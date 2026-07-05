#ifndef _COMPANION_HTTP_CONTROL_H_
#define _COMPANION_HTTP_CONTROL_H_

#include <cstddef>
#include <functional>
#include <string>

// CompanionHttpControl 设备控制面 HTTP RPC 客户端(对齐 backend proto/companion_device.proto:GetPairingState/ReportDeviceStatus/CheckFirmwareUpdate)。
//
// 三条 RPC(均 POST + 请求头 Authorization: Bearer <设备令牌>,无签名——SignCheck 豁免、DeviceAuth 验令牌):
//   - GetPairingState:未绑定期短轮询拿配对码 / 绑定态。
//   - ReportDeviceStatus:心跳 + 状态快照(全生命周期持续,周期 = Bootstrap 下发的 heartbeatSeconds)。
//   - CheckFirmwareUpdate:OTA 拉模型(回 hasUpdate + url/hash/size → 现有 CompanionOta::Upgrade)。
//
// 与 /agent/v1 WSS 正交:不依赖 WS 在连。令牌遇 HTTP 401 → token_valid()=false,上层据此重 Bootstrap。
//
// 这是 Tier-1 全隔离新文件(零 upstream 冲突):所有控制面逻辑收口于此,不碰共享核心。
// HTTP 用法完全复刻 companion_protocol.cc 的 Bootstrap(CreateHttp + SetHeader + SetContent + Open/ReadAll)。
class CompanionHttpControl {
public:
    // GetPairingState 响应解析结果。
    struct PairingState {
        bool bound = false;            // phase == "PAIRING_PHASE_BOUND"
        std::string pairing_code;      // phase == PAIRING 时的 8 位一次性配对码
        int ttl_seconds = 0;           // 配对码剩余秒数(int32)
        std::string message;           // 展示文案
    };

    // CheckFirmwareUpdate 响应解析结果。
    struct UpdateInfo {
        bool has_update = false;
        std::string version;           // 目标版本
        std::string package_url;       // OSS 直链
        std::string package_hash;      // sha256 hex
        size_t package_size = 0;       // 字节数(0 = 不校验大小)
        bool force_update = false;
    };

    // 单例(随 Application 生命周期)。
    static CompanionHttpControl& GetInstance();

    // 用 Bootstrap 结果初始化/刷新令牌 + 控制面 base + 心跳周期。每次 Bootstrap 成功后调。
    //   bearer_token    Bootstrap 返回的 data.deviceToken。
    //   base_url        控制面 base,形如 "https://api.lmcl.xyz/grpc-gateway/CompanionDeviceService"(末尾无 /)。
    //                   由 Bootstrap 下发的 websocket 地址推导(同 host)+ 固定路径后缀。
    //   heartbeat_seconds = Bootstrap 下发的 config.heartbeatSeconds(默认 60)。
    void Configure(const std::string& bearer_token, const std::string& base_url, int heartbeat_seconds);
    void SetRefreshCallback(std::function<bool()> callback);

    // GetPairingState:成功(phase 解析)返回 true 并填 out;HTTP 非 200 / 信封 code!=0 / 解析异常返回 false。
    // 遇 401 置 token_valid()=false(返回 false)。
    bool GetPairingState(PairingState& out);

    // ReportDeviceStatus:心跳。状态快照在内部组装(firmwareVersion / deviceModelCode / powerSource /
    // batteryLevel / extra{boardName,mac},字段对齐 backend companion_device.proto ReportDeviceStatus)。成功(HTTP 200 且 code==0)返回 true。
    bool ReportDeviceStatus();

    // CheckFirmwareUpdate:成功返回 true 并填 out;hasUpdate=false 时 out 其余字段为空。
    bool CheckFirmwareUpdate(UpdateInfo& out);

    bool token_valid() const { return token_valid_; }
    bool configured() const { return configured_; }
    int heartbeat_seconds() const { return heartbeat_seconds_; }

private:
    CompanionHttpControl() = default;

    // PostRpc 统一 POST + Bearer + Content-Type:application/json。
    //   method:RPC 名("GetPairingState" / "ReportDeviceStatus" / "CheckFirmwareUpdate")。
    //   body_json:请求体(裸 proto message 字段,非 {type,data} 信封)。
    //   response_out:HTTP 200 时的响应体(正常信封 {code,message,data})。
    // 返回 HTTP 状态码(200/401/403/429/500/...);Open 失败返回 -1。遇 401 置 token_valid_=false。
    int PostRpc(const std::string& method, const std::string& body_json, std::string& response_out);

    std::string bearer_token_;
    std::string base_url_;       // .../grpc-gateway/CompanionDeviceService(末尾无 /)
    int heartbeat_seconds_ = 60;
    bool configured_ = false;
    bool token_valid_ = true;
    std::function<bool()> refresh_callback_;
};

#endif // _COMPANION_HTTP_CONTROL_H_
