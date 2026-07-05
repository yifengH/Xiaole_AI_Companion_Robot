#include "companion_protocol.h"
#include "companion_http_control.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"
#include "device_identity.h"
#include "display.h"

#include <cstring>
#include <cstdlib>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "assets/lang_config.h"

#define TAG "Companion"

namespace {
constexpr size_t kMaxVisionFrameBytes = 512 * 1024;
constexpr size_t kMaxCameraNameBytes = 64;
}

CompanionProtocol::CompanionProtocol() {
    event_group_handle_ = xEventGroupCreate();
    server_sample_rate_ = 24000;
    server_frame_duration_ = 20;
}

CompanionProtocol::~CompanionProtocol() {
    CompanionHttpControl::GetInstance().SetRefreshCallback(nullptr);
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

// StripLastPathSegment 去掉 URL 末尾路径段(.../CompanionDeviceService/Bootstrap → .../CompanionDeviceService)。
// 用于从 Bootstrap 地址推导 HTTP 控制面 base(同 host + /grpc-gateway/CompanionDeviceService)。
std::string StripLastPathSegment(const std::string& url) {
    size_t qpos = url.find('?');
    std::string path = (qpos == std::string::npos) ? url : url.substr(0, qpos);
    while (!path.empty() && path.back() == '/') {
        path.pop_back();  // 去尾部斜杠
    }
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos || pos == 0) {
        return path;  // 异常(无段可去):原样返回,调用方拼出的 URL 会失败但不崩。
    }
    return path.substr(0, pos);
}
}

bool CompanionProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool CompanionProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!voice_enabled_) {
        return true;  // 服务端禁语音:静默吞掉(视为已消费),不上行服务端不会消费的音频
    }
    // 上行二进制帧:首字节 0x01 = Opus,其后是裸 Opus 包(无 turnId / 版本号 / 长度头)。
    // (下行才带 turnId 低 8 位;上行设备不知 turnId,只发 [0x01][Opus]。)
    std::string serialized;
    serialized.resize(1 + packet->payload.size());
    serialized[0] = 0x01;
    memcpy(serialized.data() + 1, packet->payload.data(), packet->payload.size());
    return websocket_->Send(serialized.data(), serialized.size(), true);
}

bool CompanionProtocol::SendImage(const std::string& camera_name, const uint8_t* jpeg, size_t len) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (jpeg == nullptr || len == 0 || len > kMaxVisionFrameBytes) {
        return false;
    }
    if (camera_name.empty() || camera_name.size() > kMaxCameraNameBytes) {
        return false;
    }

    // 上行二进制帧:[0x02][cameraNameLen uint8][cameraName UTF-8][JPEG]。
    std::string serialized;
    serialized.resize(2 + camera_name.size() + len);
    serialized[0] = 0x02;
    serialized[1] = static_cast<char>(camera_name.size());
    memcpy(serialized.data() + 2, camera_name.data(), camera_name.size());
    memcpy(serialized.data() + 2 + camera_name.size(), jpeg, len);
    return websocket_->Send(serialized.data(), serialized.size(), true);
}

bool CompanionProtocol::SendText(const std::string& text) {
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

bool CompanionProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

bool CompanionProtocol::IsTimeout() const {
    // 常驻连接:设备不主动 ping,靠服务端每 ~60s 的 WS ping + 库自动 pong 维持 TCP。
    // 这里的应用层超时只兜底「真死连接」,须显著大于契约的 ~150s 离线窗口,避免空闲时误判触发重连 churn。
    // ⚠️ 待硬件验证:底层 WebSocket 库收到服务端 ping 时是否刷新 last_incoming_time_(经 OnData)。
    //    若不刷新,长时间空闲仍会在此超时——届时应在库层把收到 ping 也算作「有活动」。
    const int kTimeoutSeconds = 300;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    return duration.count() > kTimeoutSeconds;
}

void CompanionProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;  // Websocket doesn't need to send goodbye message
    websocket_.reset();
}

bool CompanionProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    turn_known_ = false;
    current_turn_low8_ = 0;
    voice_enabled_ = true;  // 每次重连重置,以新会话 ready.voiceEnabled 为准
    xEventGroupClearBits(event_group_handle_, COMPANION_PROTOCOL_SERVER_READY_EVENT);

    // Plan 6 A:出厂预登记身份前置。未配置(无出厂 sn/secret)不连服务器,屏显提示。
    if (!DeviceIdentity::IsProvisioned()) {
        ESP_LOGE(TAG, "Device NOT provisioned — refusing to Bootstrap");
        SetError("设备未出厂配置,请联系产线");
        return false;
    }

    // 1. Bootstrap:换令牌 + 下发 /agent/v1 地址 + 心跳周期;同时 Configure HTTP 控制面。
    std::string url;
    if (!Bootstrap(url)) {
        return false;
    }

    // 首跳心跳:立即上报一次(设备上线 + 型号归属);配对期间主循环被 WaitForBound 占用,由它维持在线新鲜度。
    CompanionHttpControl::GetInstance().ReportDeviceStatus();

    // 2. /agent/v1 仅绑定后准入:未绑定先 HTTP GetPairingState 轮询直到 phase=BOUND(屏显配对码)。
    if (!WaitForBound()) {
        return false;
    }

    // 3. 连 /agent/v1 WSS(令牌已由 Bootstrap 拼进 url 的 ?token=)。
    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // 下行二进制帧:[0x01][turnId 低8位][Opus 24k]。turnId 低8位 != 当前轮 → 丢弃(作废轮迟到音频)。
            if (on_incoming_audio_ != nullptr && len > 2 && static_cast<uint8_t>(data[0]) == 0x01) {
                uint8_t low8 = static_cast<uint8_t>(data[1]);
                if (turn_known_ && low8 != current_turn_low8_) {
                    return;  // 作废轮的迟到音频,丢
                }
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>((uint8_t*)data + 2, (uint8_t*)data + len)
                }));
            }
        } else {
            // 文本帧:统一信封 {type, data}。ready 在此就地处理(采样率 + 开闸);其余转交 application。
            auto root = cJSON_ParseWithLength(data, len);
            if (root == nullptr) {
                ESP_LOGW(TAG, "Invalid JSON frame: %s", std::string(data, len).c_str());
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                auto data_obj = cJSON_GetObjectItem(root, "data");
                if (strcmp(type->valuestring, "ready") == 0) {
                    ParseServerReady(root);
                } else if (!ShouldDropStaleTextFrame(type->valuestring, data_obj)) {
                    // stt/subtitle/audioStart/turnEnd/intent/error 交 application 处理。
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

    ESP_LOGI(TAG, "Connecting to /agent/v1: %s", websocket_url_.c_str());
    if (!websocket_->Connect(websocket_url_.c_str())) {
        // 底层 WebSocket 组件未透出握手 HTTP 状态码(401/403/503);每次连接前已重 Bootstrap,令牌恒新。
        // 被封禁/吊销会在握手被拒(连接失败)→ 上层退避重连(不狂连)。
        ESP_LOGE(TAG, "Failed to connect to /agent/v1, code=%d", websocket_->GetLastError());
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    // 4. 等服务端 ready(语音就绪 + 下行采样率)。
    constexpr int kReadyWaitSeconds = 30;
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, COMPANION_PROTOCOL_SERVER_READY_EVENT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(kReadyWaitSeconds * 1000));
    if (!(bits & COMPANION_PROTOCOL_SERVER_READY_EVENT)) {
        ESP_LOGE(TAG, "Timed out waiting for server ready");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

void CompanionProtocol::ParseServerReady(const cJSON* root) {
    // ready.data:{audio:{format,sampleRate},voiceEnabled}。下行采样率以 audio.sampleRate 为准。
    auto data_obj = cJSON_GetObjectItem(root, "data");
    auto audio = cJSON_GetObjectItem(data_obj, "audio");
    if (cJSON_IsObject(audio)) {
        auto sample_rate = cJSON_GetObjectItem(audio, "sampleRate");
        if (cJSON_IsNumber(sample_rate)) {
            server_sample_rate_ = sample_rate->valueint;
        }
    }
    // voiceEnabled=false:本会话语音面被服务端禁用 → 不发 listen、丢上行音频(见 SendStartListening/SendAudio)。
    auto voice_enabled = cJSON_GetObjectItem(data_obj, "voiceEnabled");
    voice_enabled_ = !cJSON_IsBool(voice_enabled) || cJSON_IsTrue(voice_enabled);
    if (!voice_enabled_) {
        ESP_LOGW(TAG, "Server ready with voiceEnabled=false — uplink audio suppressed for this session");
    }
    ESP_LOGI(TAG, "Server ready: sampleRate=%d, frameDuration=%d, voiceEnabled=%d",
        server_sample_rate_, server_frame_duration_, voice_enabled_);
    xEventGroupSetBits(event_group_handle_, COMPANION_PROTOCOL_SERVER_READY_EVENT);
}

bool CompanionProtocol::ShouldDropStaleTextFrame(const char* type, const cJSON* data_obj) {
    if (!cJSON_IsObject(data_obj)) {
        return false;
    }
    auto turn_id = cJSON_GetObjectItem(data_obj, "turnId");
    if (!cJSON_IsString(turn_id) || turn_id->valuestring == nullptr) {
        return false;
    }

    long long id = strtoll(turn_id->valuestring, nullptr, 10);
    uint8_t low8 = static_cast<uint8_t>(id & 0xff);
    bool starts_new_turn = strcmp(type, "audioStart") == 0 ||
                           strcmp(type, "turnStart") == 0 ||
                           strcmp(type, "stt") == 0;
    if (!turn_known_ || starts_new_turn) {
        current_turn_low8_ = low8;
        turn_known_ = true;
        return false;
    }

    if (low8 != current_turn_low8_) {
        ESP_LOGW(TAG, "Drop stale %s frame turnId=%lld currentLow8=%u",
                 type, id, current_turn_low8_);
        return true;
    }

    return false;
}

bool CompanionProtocol::WaitForBound() {
    // /agent/v1 仅绑定后准入:未绑定时 HTTP 短轮询 GetPairingState,屏显配对码 + 倒计时,直到 phase=BOUND。
    // (取代旧 WSS pairingCode/bound 帧 + NotifyDeviceBound 反向回调。)
    auto display = Board::GetInstance().GetDisplay();
    constexpr int kPollIntervalSec = 2;
    constexpr int kMaxWaitSec = 10 * 60;
    // 「真没绑,等人配对」才值得占满 10 分钟;「轮询本身连不上」(网络抖动/服务不可达)必须快速失败——
    // 本函数在主事件循环里同步跑,耗在这儿按键/唤醒词/心跳全停摆;失败退出后由上层指数退避重连。
    constexpr int kMaxConsecutiveFailures = 5;
    int consecutive_failures = 0;
    for (int waited = 0; waited < kMaxWaitSec; waited += kPollIntervalSec) {
        int heartbeat_seconds = CompanionHttpControl::GetInstance().heartbeat_seconds();
        if (heartbeat_seconds > 0 && waited > 0 && (waited % heartbeat_seconds) == 0) {
            CompanionHttpControl::GetInstance().ReportDeviceStatus();
        }

        // 令牌 401(GetPairingState 把 token_valid_ 置 false)→ 上层重 Bootstrap(重试整个 OpenAudioChannel)。
        if (!CompanionHttpControl::GetInstance().token_valid()) {
            ESP_LOGW(TAG, "Control-plane token invalid (401) — need re-Bootstrap");
            return false;
        }
        CompanionHttpControl::PairingState st;
        if (CompanionHttpControl::GetInstance().GetPairingState(st)) {
            consecutive_failures = 0;
            if (st.bound) {
                ESP_LOGI(TAG, "Device bound — proceeding to /agent/v1");
                display->SetChatMessage("system", "设备已绑定,连接语音服务...");
                return true;
            }
            // PAIRING:屏显配对码 + 倒计时(码惰性轮转:轮询拿新码即更新显示)。
            std::string msg = "配对码: ";
            msg += st.pairing_code.empty() ? std::string("(等待)") : st.pairing_code;
            if (st.ttl_seconds > 0) {
                msg += " / " + std::to_string(st.ttl_seconds) + "s";
            }
            display->SetChatMessage("system", msg.c_str());
        } else {
            ESP_LOGW(TAG, "GetPairingState failed during pairing poll");
            if (++consecutive_failures >= kMaxConsecutiveFailures) {
                ESP_LOGE(TAG, "GetPairingState failed %d times in a row — bail out, let backoff reconnect", consecutive_failures);
                SetError(Lang::Strings::SERVER_NOT_CONNECTED);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollIntervalSec * 1000));
    }
    ESP_LOGE(TAG, "Timed out waiting for device binding");
    SetError(Lang::Strings::SERVER_TIMEOUT);
    return false;
}

bool CompanionProtocol::Bootstrap(std::string& websocket_url) {
    Settings settings("websocket", false);
    std::string bootstrap_url = settings.GetString("bootstrap_url", CONFIG_DEVICE_BOOTSTRAP_URL);
    std::string sn = DeviceIdentity::GetSerialNumber();
    std::string secret = DeviceIdentity::GetSecret();

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");

    cJSON* request = cJSON_CreateObject();
    // orgId 是 int64(>2^53):必须以 JSON 字符串发送(protojson 约定;若发 number 会因 double 丢精度)。
    cJSON_AddStringToObject(request, "orgId", DeviceIdentity::GetOrgId().c_str());
    cJSON_AddStringToObject(request, "sn", sn.c_str());
    cJSON_AddStringToObject(request, "secret", secret.c_str());
    http->SetContent(JsonToString(request));

    ESP_LOGI(TAG, "Bootstrap: %s, orgId=%s, sn=%s", bootstrap_url.c_str(),
             DeviceIdentity::GetOrgId().c_str(), sn.c_str());
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

    bool ok = false;
    auto code = cJSON_GetObjectItem(root, "code");
    auto message = cJSON_GetObjectItem(root, "message");
    auto data = cJSON_GetObjectItem(root, "data");
    if (cJSON_IsNumber(code) && code->valueint == 0 && cJSON_IsObject(data)) {
        auto token = cJSON_GetObjectItem(data, "deviceToken");
        auto config = cJSON_GetObjectItem(data, "config");
        auto endpoints = cJSON_IsObject(config) ? cJSON_GetObjectItem(config, "endpoints") : nullptr;
        auto ws = cJSON_IsObject(endpoints) ? cJSON_GetObjectItem(endpoints, "websocket") : nullptr;
        auto hb = cJSON_IsObject(config) ? cJSON_GetObjectItem(config, "heartbeatSeconds") : nullptr;
        if (cJSON_IsString(token) && cJSON_IsString(ws)) {
            websocket_url = AppendTokenToUrl(ws->valuestring, token->valuestring);
            websocket_url_ = websocket_url;
            int heartbeat_seconds = cJSON_IsNumber(hb) ? hb->valueint : 60;
            // Configure HTTP 控制面(配对轮询 / 心跳 / OTA):令牌 + 控制面 base(从 Bootstrap 地址推导)+ 心跳周期。
            std::string control_base = StripLastPathSegment(bootstrap_url);
            CompanionHttpControl::GetInstance().Configure(token->valuestring, control_base, heartbeat_seconds);
            CompanionHttpControl::GetInstance().SetRefreshCallback([this]() {
                return Bootstrap(websocket_url_);
            });
            ESP_LOGI(TAG, "Bootstrap ok: agent ws=%s, heartbeat=%ds, control=%s",
                     ws->valuestring, heartbeat_seconds, control_base.c_str());
            ok = true;
        } else {
            ESP_LOGE(TAG, "Bootstrap response missing deviceToken or websocket");
            SetError(Lang::Strings::SERVER_ERROR);
        }
    } else {
        ESP_LOGE(TAG, "Bootstrap rejected: %s", cJSON_IsString(message) ? message->valuestring : response.c_str());
        SetError(cJSON_IsString(message) ? message->valuestring : Lang::Strings::SERVER_ERROR);
    }

    cJSON_Delete(root);
    return ok;
}

void CompanionProtocol::SendStartListening(ListeningMode mode) {
    // 契约 §4.1:listen 仅 {type:"listen",data:{state:"start"}}。mode 是端侧本地概念,不进 wire。
    (void)mode;
    if (!voice_enabled_) {
        ESP_LOGW(TAG, "voiceEnabled=false — skip listen frame (server-side voice disabled)");
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "state", "start");
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void CompanionProtocol::SendAbortSpeaking(AbortReason reason) {
    // Plan 6 D7:手停纯本地(停渲染下行音频/字幕 + Abort 本地播放器),不发任何上行帧。
    // 服务端靠限速下发 + 端侧 ~200ms 小缓冲自动收口 barge-in;上行 abort 反会误杀刚起的新轮。
    // 本地停播由 Application::AbortSpeaking 完成(置 aborted_、ResetDecoder),不依赖本方法。
    (void)reason;
}

void CompanionProtocol::SendDeviceEvent(const std::string& event, const std::string& instruction) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "deviceEvent");
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(data, "event", event.c_str());
    cJSON_AddStringToObject(data, "source", "bmi270");
    cJSON_AddStringToObject(data, "text", instruction.c_str());
    cJSON_AddStringToObject(data, "instruction", instruction.c_str());
    cJSON_AddBoolToObject(data, "requestReply", true);
    cJSON_AddItemToObject(root, "data", data);
    SendText(JsonToString(root));
}

void CompanionProtocol::SendMcpMessage(const std::string& payload) {
    cJSON* payload_json = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (payload_json == nullptr) {
        ESP_LOGE(TAG, "Invalid MCP payload JSON: %s", payload.c_str());
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload_json);
    SendText(JsonToString(root));
}
