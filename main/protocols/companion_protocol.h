#ifndef _COMPANION_PROTOCOL_H_
#define _COMPANION_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define COMPANION_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)

class CompanionProtocol : public Protocol {
public:
    CompanionProtocol();
    ~CompanionProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    void SendStartListening(ListeningMode mode) override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendDeviceStatus() override;
    void SendDeviceEvent(const std::string& event, const std::string& instruction) override;
    // MCP 设备能力工具通道:契约用 {type:"mcp",payload} 信封(无 session_id),覆盖基类默认格式。
    // 当前后端契约未启用 mcp 帧,保留设备侧管道以便将来接入。
    void SendMcpMessage(const std::string& payload) override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    bool saw_pairing_code_ = false;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    // 常驻连接专用超时(显著大于契约 ~150s 离线窗口),覆盖基类默认的 120s。
    bool IsTimeout() const override;
    std::string GetStatusMessage();
    std::string GetCheckUpdateMessage();
    bool Bootstrap(std::string& websocket_url);
};

#endif
