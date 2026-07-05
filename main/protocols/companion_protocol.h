#ifndef _COMPANION_PROTOCOL_H_
#define _COMPANION_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#define COMPANION_PROTOCOL_SERVER_READY_EVENT (1 << 0)

// CompanionProtocol 设备实时语音协议(/agent/v1,对齐 backend proto/agent_realtime/agent_realtime_core_model.proto + Plan 6)。
//
// 两段式(控制面已移出本 WSS,走 HTTP):
//  1. Bootstrap(HTTP)换设备令牌 + 下发 /agent/v1 地址 + 心跳周期;同时 Configure HTTP 控制面
//     (配对轮询 / 心跳 / OTA,见 CompanionHttpControl)。
//  2. 仅绑定后连 /agent/v1 WSS:未绑定先 HTTP GetPairingState 轮询(显示配对码),phase=BOUND 才连 WS;
//     连后等服务端 ready(语音就绪 + 下行采样率)。
//
// 本 WSS 只承载纯对话帧(ready/stt/subtitle/audioStart/turnEnd/intent/error + 二进制音频);
// status/checkUpdate/pairingCode/bound/update/abort 帧**已移除**(走 HTTP 控制面 / 手停本地)。
class CompanionProtocol : public Protocol {
public:
    CompanionProtocol();
    ~CompanionProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool SendImage(const std::string& camera_name, const uint8_t* jpeg, size_t len) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    void SendStartListening(ListeningMode mode) override;
    // Plan 6 D7:手停纯本地(停渲染下行音频/字幕 + Abort 本地播放器),不发任何上行帧。
    void SendAbortSpeaking(AbortReason reason) override;
    void SendDeviceEvent(const std::string& event, const std::string& instruction) override;
    // MCP 设备能力工具通道:{type:"mcp",payload} 信封(无 session_id),覆盖基类默认格式。
    // 当前后端 /agent/v1 未处理 mcp 入站帧(静默忽略),保留设备侧管道以便将来接入。
    void SendMcpMessage(const std::string& payload) override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    std::string websocket_url_;

    // turnId 跟踪:下行二进制音频帧 = [0x01][turnId 低8位][Opus];非当前轮的迟到帧据此丢弃。
    // current_turn_low8_ 由下行文本帧的 data.turnId(int64 → 取低 8 位)更新。
    uint8_t current_turn_low8_ = 0;
    bool turn_known_ = false;

    // 服务端 ready.voiceEnabled:false = 本会话语音面被禁(如租户策略纯文字)。
    // 端侧收敛在协议层:不发 listen 帧、丢弃上行音频(服务端不会消费)。默认 true。
    bool voice_enabled_ = true;

    void ParseServerReady(const cJSON* root);
    bool SendText(const std::string& text) override;
    // 常驻连接专用超时(显著大于契约 ~150s 离线窗口),覆盖基类默认的 120s。
    bool IsTimeout() const override;
    // Bootstrap 换令牌 + 下发 /agent/v1 地址 + 心跳周期;同时 Configure HTTP 控制面。成功返回 true。
    bool Bootstrap(std::string& websocket_url);
    // 未绑定时 HTTP GetPairingState 轮询直到 phase=BOUND(屏显配对码/倒计时)。已绑定/超时→true/false。
    bool WaitForBound();
    // Returns true when a text frame belongs to an obsolete turn and should be ignored.
    bool ShouldDropStaleTextFrame(const char* type, const cJSON* data_obj);
};

#endif
