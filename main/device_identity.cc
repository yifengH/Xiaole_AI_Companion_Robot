#include "device_identity.h"

#include <esp_log.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include <sdkconfig.h>
#include <cstring>

#define TAG "DeviceIdentity"

#define LMCL_SN_SALT     "lmcl-sn-salt-CHANGE-ME-3f9a1c7e2b8d4056"
#define LMCL_SECRET_SALT "lmcl-secret-salt-CHANGE-ME-a5c9f2d7e1b6480c"

#ifndef CONFIG_LMCL_SN_PREFIX
#define CONFIG_LMCL_SN_PREFIX "lmcl-"
#endif

namespace {

bool ReadBaseMac(uint8_t mac[6]) {
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read base MAC: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

std::string HashHex(const uint8_t mac[6], const char* salt) {
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, mac, 6);
    mbedtls_sha256_update(&ctx, reinterpret_cast<const uint8_t*>(salt), strlen(salt));
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; i++) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0f]);
    }
    return out;
}

} // namespace

const std::string& DeviceIdentity::GetSerialNumber() {
    static std::string sn;
    if (!sn.empty()) {
        return sn;
    }

    uint8_t mac[6] = {0};
    ReadBaseMac(mac);
    sn = std::string(CONFIG_LMCL_SN_PREFIX) + HashHex(mac, LMCL_SN_SALT).substr(0, 24);
    ESP_LOGI(TAG, "Serial Number: %s", sn.c_str());
    return sn;
}

const std::string& DeviceIdentity::GetSecret() {
    static std::string secret;
    if (!secret.empty()) {
        return secret;
    }

    uint8_t mac[6] = {0};
    ReadBaseMac(mac);
    secret = HashHex(mac, LMCL_SECRET_SALT);
    return secret;
}
