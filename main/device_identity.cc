#include "device_identity.h"

#include <esp_log.h>
#include <sdkconfig.h>

#include "settings.h"

#define TAG "DeviceIdentity"

// 出厂身份契约(对齐 backend companion_device.proto Bootstrap TOFU):
//   - 产线出厂时把 (orgId, sn, 出厂密码) 烧进 NVS 命名空间 "identity"(键 orgId/sn/secret)。
//   - 设备**只读**出厂值,**不自生成**。首次 Bootstrap 用 (orgId,sn,secret) 做 TOFU 建档。
//
// 无出厂值 = 未配置:IsProvisioned()==false(orgId/sn/secret 任缺),调用方须进错误态、不 Bootstrap。
//
// ⚠️ 量产硬化(§7 checklist):secret 存 NVS 在「恢复出厂/整片擦除」时会被清(=失身份、原绑定丢失)。
//    量产正式应烧 efuse 用户块或独立只读分区;当前未上市,NVS 已满足安全与可联调。

namespace {

constexpr char kIdentityNamespace[] = "identity";
constexpr char kOrgIdKey[] = "orgId";
constexpr char kSnKey[] = "sn";
constexpr char kSecretKey[] = "secret";

// LoadFactory 只读 NVS 里的出厂值(不生成)。无值返回空串。
std::string LoadFactory(const char* key) {
    Settings settings(kIdentityNamespace, true);
    return settings.GetString(key);
}

} // namespace

bool DeviceIdentity::IsProvisioned() {
    return !GetOrgId().empty() && !GetSerialNumber().empty() && !GetSecret().empty();
}

const std::string& DeviceIdentity::GetOrgId() {
    static std::string org_id;
    if (org_id.empty()) {
        org_id = LoadFactory(kOrgIdKey);
        if (!org_id.empty()) {
            ESP_LOGI(TAG, "Org Id: %s", org_id.c_str());
        } else {
            ESP_LOGE(TAG, "Device NOT provisioned: factory orgId missing in NVS(%s/%s)",
                     kIdentityNamespace, kOrgIdKey);
        }
    }
    return org_id;
}

const std::string& DeviceIdentity::GetSerialNumber() {
    static std::string sn;
    if (sn.empty()) {
        sn = LoadFactory(kSnKey);
        if (!sn.empty()) {
            ESP_LOGI(TAG, "Serial Number: %s", sn.c_str());
        } else {
            ESP_LOGE(TAG, "Device NOT provisioned: factory sn missing in NVS(%s/%s)",
                     kIdentityNamespace, kSnKey);
        }
    }
    return sn;
}

const std::string& DeviceIdentity::GetSecret() {
    static std::string secret;
    if (secret.empty()) {
        secret = LoadFactory(kSecretKey);
        if (secret.empty()) {
            ESP_LOGE(TAG, "Device NOT provisioned: factory secret missing in NVS(%s/%s)",
                     kIdentityNamespace, kSecretKey);
        }
    }
    return secret;
}
