#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "device_identity.h"
#include "display.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();
    server_sample_rate_ = 24000;
    server_frame_duration_ = 20;
}

WebsocketProtocol::~WebsocketProtocol() {
    vEventGroupDelete(event_group_handle_);
}

namespace {
std::string JsonToString(cJSON* root) {
    char* json = cJSON_PrintUnformatted(root);
    if (json == nullptr) {
        cJSON_Delete(root);
        return "";
    }
    std::string result(json);
    cJSON_free(json);
    cJSON_Delete(root);
    return result;
}

std::string UrlEncode(const std::string& value) {
    static const char* hex = "0123456789ABCDEF";
    std::string escaped;
    escaped.reserve(value.size());
    for (uint8_t ch : value) {
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
            ch == '.' || ch == '~') {
            escaped.push_back(static_cast<char>(ch));
        } else {
            escaped.push_back('%');
            escaped.push_back(hex[ch >> 4]);
            escaped.push_back(hex[ch & 0x0F]);
        }
    }
    return escaped;
}

std::string AppendTokenToUrl(const std::string& url, const std::string& token) {
    std::string result = url;
    result += (result.find('?') == std::string::npos) ? "?token=" : "&token=";
    result += UrlEncode(token);
    return result;
}

const cJSON* MessageDataOrRoot(const cJSON* root) {
    auto data = cJSON_GetObjectItem(root, "data");
    return cJSON_IsObject(data) ? data : root;
}
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    // 契约二进制帧:首字节 0x01 = Opus 音频,其后是裸 Opus 包(无版本号/时间戳/长度头)。
    std::string serialized;
    serialized.resize(1 + packet->payload.size());
    serialized[0] = 0x01;
    memcpy(serialized.data() + 1, packet->payload.data(), packet->payload.size());
    return websocket_->Send(serialized.data(), serialized.size(), true);
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    websocket_.reset();
}

bool WebsocketProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    saw_pairing_code_ = false;
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    // 每次连接前都先 Bootstrap 换取新短时令牌 + 下发的 WSS 地址(契约:开机/重连第一件事)。
    std::string url;
    if (!Bootstrap(url)) {
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }
    // 鉴权只有一种:令牌已由 Bootstrap 拼进 url 的 ?token=。不发任何额外 HTTP 头。

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // 契约二进制帧:首字节是类型。0x01 = 下行 Opus 音频,其后为裸 Opus 包。
            if (on_incoming_audio_ != nullptr && len > 1 && static_cast<uint8_t>(data[0]) == 0x01) {
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>((uint8_t*)data + 1, (uint8_t*)data + len)
                }));
            }
        } else {
            // 文本帧:统一信封 {type, data}。hello/pairingCode/bound 在此就地处理(让 OpenAudioChannel 的
            // 等 hello 循环能感知配对态),其余转交 application 的 OnIncomingJson。
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGW(TAG, "Invalid JSON frame: %s", std::string(data, len).c_str());
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else if (strcmp(type->valuestring, "pairingCode") == 0) {
                    saw_pairing_code_ = true;
                    auto data_obj = MessageDataOrRoot(root);
                    auto code = cJSON_GetObjectItem(data_obj, "code");
                    auto ttl = cJSON_GetObjectItem(data_obj, "ttlSeconds");
                    std::string message = "Pairing code: ";
                    message += cJSON_IsString(code) ? code->valuestring : "(unknown)";
                    // ttlSeconds 是 int64,protojson 编码为 JSON 字符串。
                    if (cJSON_IsString(ttl)) {
                        message += " / ";
                        message += ttl->valuestring;
                        message += "s";
                    }
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", message.c_str());
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                } else if (strcmp(type->valuestring, "bound") == 0) {
                    Board::GetInstance().GetDisplay()->SetChatMessage("system", "Device bound, waiting for voice service...");
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "Missing message type, data: %s", std::string(data, len).c_str());
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    ESP_LOGI(TAG, "Connecting to websocket server: %s", url.c_str());
    if (!websocket_->Connect(url.c_str())) {
        // 注:底层 WebSocket 组件未透出握手 HTTP 状态码(401/403/503),无法在此细分处理。
        // 因每次连接前都重新 Bootstrap,令牌恒为新,401(令牌过期)已基本规避;403(未注册/封禁)/503
        // 表现为连接失败,由上层退避重连兜底。如需按状态码区分,须先让该组件透出 GetStatusCode()。
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // 连上后:上报一次状态(供后台在线/型号归属)+ 检测固件更新。设备 hello 可选,不发。
    SendText(GetStatusMessage());
    SendText(GetCheckUpdateMessage());

    // Wait for server hello
    bool hello_received = false;
    const int max_wait_seconds = 10 * 60;
    for (int waited = 0; waited < max_wait_seconds; waited += 10) {
        EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT) {
            hello_received = true;
            break;
        }
        if (!saw_pairing_code_) {
            ESP_LOGE(TAG, "Failed to receive server hello");
            SetError(Lang::Strings::SERVER_TIMEOUT);
            return false;
        }
        ESP_LOGI(TAG, "Waiting for device binding before server hello...");
    }

    if (!hello_received && saw_pairing_code_) {
        ESP_LOGE(TAG, "Timed out waiting for device binding");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    // 契约 hello.data:{ sessionId, audio:{ format, sampleRate } }(字段名一律 lowerCamelCase)。
    auto data_obj = MessageDataOrRoot(root);

    auto session_id = cJSON_GetObjectItem(data_obj, "sessionId");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio = cJSON_GetObjectItem(data_obj, "audio");
    if (cJSON_IsObject(audio)) {
        auto sample_rate = cJSON_GetObjectItem(audio, "sampleRate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
    }

    ESP_LOGI(TAG, "Server audio: sampleRate=%d, frameDuration=%d",
        server_sample_rate_, server_frame_duration_);

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

bool WebsocketProtocol::Bootstrap(std::string& websocket_url) {
    Settings settings("websocket", false);
    std::string bootstrap_url = settings.GetString("bootstrap_url", CONFIG_DEVICE_BOOTSTRAP_URL);
    std::string sn = settings.GetString("sn", DeviceIdentity::GetSerialNumber());
    std::string secret = settings.GetString("secret", DeviceIdentity::GetSecret());

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");

    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "sn", sn.c_str());
    cJSON_AddStringToObject(request, "secret", secret.c_str());
    http->SetContent(JsonToString(request));

    ESP_LOGI(TAG, "Bootstrap: %s, sn=%s", bootstrap_url.c_str(), sn.c_str());
    if (!http->Open("POST", bootstrap_url)) {
        ESP_LOGE(TAG, "Failed to open bootstrap HTTP connection, code=0x%x", http->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Bootstrap failed, status=%d, body=%s", status_code, response.c_str());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    cJSON* root = cJSON_Parse(response.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Invalid bootstrap response: %s", response.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    auto code = cJSON_GetObjectItem(root, "code");
    auto message = cJSON_GetObjectItem(root, "message");
    auto data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsNumber(code) || code->valueint != 0 || !cJSON_IsObject(data)) {
        ESP_LOGE(TAG, "Bootstrap rejected: %s", cJSON_IsString(message) ? message->valuestring : response.c_str());
        cJSON_Delete(root);
        SetError(cJSON_IsString(message) ? message->valuestring : Lang::Strings::SERVER_ERROR);
        return false;
    }

    auto token = cJSON_GetObjectItem(data, "deviceToken");
    auto config = cJSON_GetObjectItem(data, "config");
    auto endpoints = cJSON_IsObject(config) ? cJSON_GetObjectItem(config, "endpoints") : nullptr;
    auto ws = cJSON_IsObject(endpoints) ? cJSON_GetObjectItem(endpoints, "websocket") : nullptr;
    if (!cJSON_IsString(token) || !cJSON_IsString(ws)) {
        ESP_LOGE(TAG, "Bootstrap response missing deviceToken or websocket");
        cJSON_Delete(root);
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    websocket_url = AppendTokenToUrl(ws->valuestring, token->valuestring);
    ESP_LOGI(TAG, "Bootstrap websocket endpoint: %s", ws->valuestring);
    cJSON_Delete(root);
    return true;
}

std::string WebsocketProtocol::GetStatusMessage() {
    Settings settings("websocket", false);
    auto app_desc = esp_app_get_description();
    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    bool has_battery = Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "status");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "firmwareVersion", app_desc->version);
    cJSON_AddStringToObject(data, "deviceModelCode", settings.GetString("model", CONFIG_DEVICE_MODEL_CODE).c_str());
    cJSON_AddNumberToObject(data, "powerSource", has_battery && discharging ? 1 : 2);
    if (has_battery) {
        cJSON_AddNumberToObject(data, "batteryLevel", battery_level);
    }
    cJSON* extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "boardName", BOARD_NAME);
    cJSON_AddStringToObject(extra, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddItemToObject(data, "extra", extra);
    cJSON_AddItemToObject(root, "data", data);
    return JsonToString(root);
}

std::string WebsocketProtocol::GetCheckUpdateMessage() {
    auto app_desc = esp_app_get_description();
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "checkUpdate");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "version", app_desc->version);
    cJSON_AddItemToObject(root, "data", data);
    return JsonToString(root);
}

void WebsocketProtocol::SendStartListening(ListeningMode mode) {
    // 契约 §4.1:listen 仅 {type:"listen",data:{state:"start"}}。mode 是端侧本地概念,不进 wire。
    (void)mode;
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "start");
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void WebsocketProtocol::SendAbortSpeaking(AbortReason reason) {
    // 契约 §6:语音/唤醒词插话打断**不发** abort(服务端自动取消旧轮,发了会误杀刚起的新轮);
    // 只有用户「按键手动停止」(kAbortReasonNone)才发 {type:"abort"}(无 data)。
    if (reason != kAbortReasonNone) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "abort");
    SendText(JsonToString(root));
}

void WebsocketProtocol::SendDeviceStatus() {
    SendText(GetStatusMessage());
}
