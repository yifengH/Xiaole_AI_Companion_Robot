#include "protocol.h"

#include <esp_log.h>

#define TAG "Protocol"

namespace {
std::string ToJsonString(cJSON* root) {
    if (root == nullptr) {
        return "";
    }
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
}

void Protocol::OnIncomingJson(std::function<void(const cJSON* root)> callback) {
    on_incoming_json_ = callback;
}

void Protocol::OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback) {
    on_incoming_audio_ = callback;
}

void Protocol::OnAudioChannelOpened(std::function<void()> callback) {
    on_audio_channel_opened_ = callback;
}

void Protocol::OnAudioChannelClosed(std::function<void()> callback) {
    on_audio_channel_closed_ = callback;
}

void Protocol::OnNetworkError(std::function<void(const std::string& message)> callback) {
    on_network_error_ = callback;
}

void Protocol::OnConnected(std::function<void()> callback) {
    on_connected_ = callback;
}

void Protocol::OnDisconnected(std::function<void()> callback) {
    on_disconnected_ = callback;
}

void Protocol::SetError(const std::string& message) {
    error_occurred_ = true;
    if (on_network_error_ != nullptr) {
        on_network_error_(message);
    }
}

void Protocol::SendMcpMessage(const std::string& payload) {
    cJSON* payload_json = cJSON_ParseWithLength(payload.c_str(), payload.size());
    if (payload_json == nullptr) {
        ESP_LOGE(TAG, "Invalid MCP payload JSON: %s", payload.c_str());
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "mcp");
    cJSON_AddItemToObject(root, "payload", payload_json);
    std::string message = ToJsonString(root);
    if (message.empty()) {
        ESP_LOGE(TAG, "Failed to build MCP message");
        return;
    }
    SendText(message);
}

bool Protocol::IsTimeout() const {
    // 常驻连接:设备不主动 ping,靠服务端每 ~60s 的 WS ping + 库自动 pong 维持 TCP。
    // 这里的应用层超时只兜底「真死连接」,须显著大于契约的 ~150s 离线窗口,避免空闲时误判触发重连churn。
    // ⚠️ 待硬件验证:底层 WebSocket 库收到服务端 ping 时是否刷新 last_incoming_time_(经 OnData)。
    //    若不刷新,长时间空闲仍会在此超时——届时应在库层把收到 ping 也算作「有活动」。
    const int kTimeoutSeconds = 300;
    auto now = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - last_incoming_time_);
    bool timeout = duration.count() > kTimeoutSeconds;
    if (timeout) {
        ESP_LOGE(TAG, "Channel timeout %ld seconds", (long)duration.count());
    }
    return timeout;
}
