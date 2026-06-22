#include "device_identity.h"

#include <esp_log.h>
#include <esp_random.h>
#include <sdkconfig.h>

#include "settings.h"

#define TAG "DeviceIdentity"

// 设备身份(sn + 出厂密码)契约要求:**每台随机、高熵、不可猜**,且是设备一辈子的身份。
// 绝不可由 MAC 派生 —— MAC 可空中嗅探/可猜,一旦有人据此抢先注册(首连即 TOFU 记录),真机就被锁死。
//
// 当前实现:首次开机用硬件真随机数(esp_random)各生成一次,持久化到 NVS,此后只读。
//   - sn     = CONFIG_LMCL_SN_PREFIX + 24 个 hex(12 字节随机)
//   - secret = 64 个 hex(32 字节随机,远超契约建议的 ≥32 字符)
// 这满足「随机、高熵、不可猜、一机一份、跨重启不变」。
//
// ⚠️ 量产须知:NVS 默认分区在「整片擦除/恢复出厂」时会被清掉,清掉后会重新随机生成(=变成一台新设备,
//   原绑定丢失)。契约要求烧在「恢复出厂擦不掉的分区」——量产时应由产线把高熵随机值预写入 efuse 用户块
//   或独立只读分区,并在此读取;读不到再退回本地生成。当前未上市,用 NVS 自生成已满足安全性与可联调。

namespace {

constexpr char kIdentityNamespace[] = "identity";
constexpr char kSnKey[] = "sn";
constexpr char kSecretKey[] = "secret";

// RandomHex 生成 n 字节硬件真随机数并转成小写 hex 字符串(长度 2n)。
std::string RandomHex(size_t bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; i++) {
        uint8_t b = static_cast<uint8_t>(esp_random() & 0xFF);
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

// LoadOrCreate 读 NVS 里的身份值;不存在则用硬件真随机数生成一次并持久化(generate-once)。
std::string LoadOrCreate(const char* key, const std::string& generated) {
    Settings settings(kIdentityNamespace, true);
    std::string value = settings.GetString(key);
    if (!value.empty()) {
        return value;
    }
    settings.SetString(key, generated);
    ESP_LOGW(TAG, "Provisioned new device %s into NVS (first boot)", key);
    return generated;
}

} // namespace

const std::string& DeviceIdentity::GetSerialNumber() {
    static std::string sn;
    if (sn.empty()) {
        sn = LoadOrCreate(kSnKey, std::string(CONFIG_LMCL_SN_PREFIX) + RandomHex(12));
        ESP_LOGI(TAG, "Serial Number: %s", sn.c_str());
    }
    return sn;
}

const std::string& DeviceIdentity::GetSecret() {
    static std::string secret;
    if (secret.empty()) {
        secret = LoadOrCreate(kSecretKey, RandomHex(32));
    }
    return secret;
}
