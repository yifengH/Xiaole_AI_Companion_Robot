#ifndef _DEVICE_IDENTITY_H_
#define _DEVICE_IDENTITY_H_

#include <string>

// DeviceIdentity 出厂身份(对齐 backend companion_device.proto Bootstrap TOFU)。
//
// 每台设备的 (orgId, sn, 出厂密码) 由产线在出厂时烧录到 NVS 命名空间 "identity"(键 orgId/sn/secret)。
// 设备**不自生成身份**;首次 Bootstrap 时 backend 用 (orgId,sn,secret) 做 TOFU 建档
// (校验 org 存在+ACTIVE → 落 sn+bcrypt(密码)+orgId),之后 orgId 一致 + 验密码换令牌。
//
// 用法:Bootstrap 前必须先查 IsProvisioned();未配置(orgId/sn/secret 任缺)则进「设备未出厂配置」错误态(屏显提示、不连服务器)。
class DeviceIdentity {
public:
    // 是否已出厂配置(NVS 既有 orgId 又有 sn 又有 secret)。未配置时 Get* 返回空串。
    static bool IsProvisioned();
    static const std::string& GetOrgId();         // 出厂烧录的归属 orgId(TOFU 首连建档据此落 org_id)
    static const std::string& GetSerialNumber();
    static const std::string& GetSecret();
};

#endif // _DEVICE_IDENTITY_H_
