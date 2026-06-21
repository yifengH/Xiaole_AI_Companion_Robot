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
#include <arpa/inet.h>
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

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        std::string serialized;
        serialized.resize(1 + packet->payload.size());
        serialized[0] = 0x01;
        memcpy(serialized.data() + 1, packet->payload.data(), packet->payload.size());
        return websocket_->Send(serialized.data(), serialized.size(), true);
    }
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
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    error_occurred_ = false;
    saw_pairing_code_ = false;
    xEventGroupClearBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);

    if (ShouldUseBootstrap()) {
        if (!Bootstrap(url)) {
            return false;
        }
        token.clear();
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGW(TAG, "Binary protocol v2 packet too short: %u", (unsigned)len);
                        return;
                    }
                    const auto* bp2 = reinterpret_cast<const BinaryProtocol2*>(data);
                    const uint32_t timestamp = ntohl(bp2->timestamp);
                    const uint32_t payload_size = ntohl(bp2->payload_size);
                    if (len < sizeof(BinaryProtocol2) + payload_size) {
                        ESP_LOGW(TAG, "Binary protocol v2 payload truncated: len=%u payload=%u", (unsigned)len, (unsigned)payload_size);
                        return;
                    }
                    const auto* payload = reinterpret_cast<const uint8_t*>(bp2->payload);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)
                    }));
                } else if (version_ == 3) {
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGW(TAG, "Binary protocol v3 packet too short: %u", (unsigned)len);
                        return;
                    }
                    const auto* bp3 = reinterpret_cast<const BinaryProtocol3*>(data);
                    const uint16_t payload_size = ntohs(bp3->payload_size);
                    if (len < sizeof(BinaryProtocol3) + payload_size) {
                        ESP_LOGW(TAG, "Binary protocol v3 payload truncated: len=%u payload=%u", (unsigned)len, (unsigned)payload_size);
                        return;
                    }
                    const auto* payload = reinterpret_cast<const uint8_t*>(bp3->payload);
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = len > 0 && static_cast<uint8_t>(data[0]) == 0x01
                            ? std::vector<uint8_t>((uint8_t*)data + 1, (uint8_t*)data + len)
                            : std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
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
                    if (cJSON_IsNumber(ttl)) {
                        message += " / ";
                        message += std::to_string(ttl->valueint);
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

    ESP_LOGI(TAG, "Connecting to websocket server: %s with version: %d", url.c_str(), version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to websocket server, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    if (!ShouldUseBootstrap() && !SendText(GetHelloMessage())) {
        return false;
    }
    SendText(GetStatusMessage());
    if (ShouldUseBootstrap()) {
        SendText(GetCheckUpdateMessage());
    }

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

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto data_obj = MessageDataOrRoot(root);
    auto transport = cJSON_GetObjectItem(data_obj, "transport");
    if (cJSON_IsString(transport) && strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "Unsupported transport: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(data_obj, "sessionId");
    if (!cJSON_IsString(session_id)) {
        session_id = cJSON_GetObjectItem(data_obj, "session_id");
    }
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    auto audio_params = cJSON_GetObjectItem(data_obj, "audio_params");
    if (!cJSON_IsObject(audio_params)) {
        audio_params = cJSON_GetObjectItem(data_obj, "audio");
    }
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (!cJSON_IsNumber(sample_rate)) {
            sample_rate = cJSON_GetObjectItem(audio_params, "sampleRate");
        }
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (!cJSON_IsNumber(frame_duration)) {
            frame_duration = cJSON_GetObjectItem(audio_params, "frameDuration");
        }
        if (cJSON_IsNumber(frame_duration)) {
            server_frame_duration_ = frame_duration->valueint;
        }
    }

    if (server_frame_duration_ <= 0) {
        server_frame_duration_ = 20;
        ESP_LOGW(TAG, "Server hello missing valid frame_duration, fallback to %d ms", server_frame_duration_);
    }

    ESP_LOGI(TAG, "Server audio params: sample_rate=%d, frame_duration=%d",
        server_sample_rate_, server_frame_duration_);

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}

bool WebsocketProtocol::ShouldUseBootstrap() const {
    Settings settings("websocket", false);
    return !settings.GetString("bootstrap_url", CONFIG_DEVICE_BOOTSTRAP_URL).empty();
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

void WebsocketProtocol::SendWakeWordDetected(const std::string& wake_word) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "detect");
    cJSON_AddStringToObject(data, "text", wake_word.c_str());
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void WebsocketProtocol::SendStartListening(ListeningMode mode) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "start");
    if (!ShouldUseBootstrap()) {
        if (mode == kListeningModeRealtime) {
            cJSON_AddStringToObject(data, "mode", "realtime");
        } else if (mode == kListeningModeAutoStop) {
            cJSON_AddStringToObject(data, "mode", "auto");
        } else {
            cJSON_AddStringToObject(data, "mode", "manual");
        }
    }
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void WebsocketProtocol::SendStopListening() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "stop");
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void WebsocketProtocol::SendAbortSpeaking(AbortReason reason) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "abort");
    if (reason == kAbortReasonWakeWordDetected) {
        cJSON* data = cJSON_CreateObject();
        cJSON_AddStringToObject(data, "reason", "wake_word_detected");
        cJSON_AddItemToObject(root, "data", data);
    }
    SendText(JsonToString(root));
}
