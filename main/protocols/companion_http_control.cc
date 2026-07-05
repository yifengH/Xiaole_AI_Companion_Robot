#include "companion_http_control.h"

#include <cstring>
#include <cstdlib>
#include <utility>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_app_desc.h>

#include "board.h"
#include "settings.h"
#include "system_info.h"

#define TAG "CompanionHttpControl"

namespace {

// cJSON 根对象 → 紧凑 JSON 字符串,打印串与 root 都在此释放(复刻 companion_protocol.cc 的同名工具)。
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

}  // namespace

CompanionHttpControl& CompanionHttpControl::GetInstance() {
    static CompanionHttpControl inst;
    return inst;
}

void CompanionHttpControl::Configure(const std::string& bearer_token,
                                     const std::string& base_url,
                                     int heartbeat_seconds) {
    bearer_token_ = bearer_token;
    base_url_ = base_url;
    heartbeat_seconds_ = heartbeat_seconds;
    configured_ = true;
    token_valid_ = true;
    ESP_LOGI(TAG, "Configured: base=%s, heartbeat=%ds", base_url_.c_str(), heartbeat_seconds_);
}

void CompanionHttpControl::SetRefreshCallback(std::function<bool()> callback) {
    refresh_callback_ = std::move(callback);
}

int CompanionHttpControl::PostRpc(const std::string& method,
                                  const std::string& body_json,
                                  std::string& response_out) {
    if (!configured_) {
        ESP_LOGE(TAG, "PostRpc(%s) called before Configure", method.c_str());
        return -1;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        std::string url = base_url_ + "/" + method;

        auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
        http->SetHeader("Content-Type", "application/json");
        std::string auth = "Bearer " + bearer_token_;
        http->SetHeader("Authorization", auth.c_str());
        http->SetContent(std::string(body_json));

        ESP_LOGI(TAG, "POST %s", url.c_str());
        if (!http->Open("POST", url)) {
            ESP_LOGE(TAG, "Failed to open HTTP connection for %s, code=0x%x",
                     method.c_str(), http->GetLastError());
            return -1;
        }

        int status_code = http->GetStatusCode();
        std::string response = http->ReadAll();
        http->Close();

        if (status_code == 401) {
            token_valid_ = false;
            ESP_LOGW(TAG, "%s rejected: 401 token invalid/expired", method.c_str());
            if (attempt == 0 && refresh_callback_ && refresh_callback_()) {
                ESP_LOGI(TAG, "Token refreshed; retrying %s once", method.c_str());
                continue;
            }
        }

        if (status_code == 200) {
            response_out = response;
        }

        ESP_LOGI(TAG, "%s status=%d", method.c_str(), status_code);
        return status_code;
    }

    return -1;
}

bool CompanionHttpControl::GetPairingState(PairingState& out) {
    std::string resp;
    int status = PostRpc("GetPairingState", "{}", resp);
    if (status != 200) {
        return false;
    }

    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "GetPairingState invalid JSON: %s", resp.c_str());
        return false;
    }

    bool ok = false;
    auto code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        ESP_LOGE(TAG, "GetPairingState envelope code!=0: %s", resp.c_str());
    } else {
        auto data = cJSON_GetObjectItem(root, "data");
        if (!cJSON_IsObject(data)) {
            ESP_LOGE(TAG, "GetPairingState missing data: %s", resp.c_str());
        } else {
            auto phase = cJSON_GetObjectItem(data, "phase");
            auto pairing_code = cJSON_GetObjectItem(data, "pairingCode");
            auto ttl = cJSON_GetObjectItem(data, "ttlSeconds");
            auto message = cJSON_GetObjectItem(data, "message");

            if (cJSON_IsString(phase)) {
                out.bound = (strcmp(phase->valuestring, "PAIRING_PHASE_BOUND") == 0);
            } else {
                out.bound = false;
            }
            out.pairing_code = cJSON_IsString(pairing_code) ? pairing_code->valuestring : "";
            out.ttl_seconds = cJSON_IsNumber(ttl) ? ttl->valueint : 0;
            out.message = cJSON_IsString(message) ? message->valuestring : "";
            ok = true;

            ESP_LOGI(TAG, "GetPairingState phase=%s code=%s ttl=%d",
                     cJSON_IsString(phase) ? phase->valuestring : "(null)",
                     out.pairing_code.c_str(), out.ttl_seconds);
        }
    }

    cJSON_Delete(root);
    return ok;
}

bool CompanionHttpControl::ReportDeviceStatus() {
    Settings settings("websocket", false);
    auto app_desc = esp_app_get_description();

    int battery_level = 0;
    bool charging = false;
    bool discharging = false;
    bool has_battery = Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging);

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "firmwareVersion", app_desc->version);
    cJSON_AddStringToObject(root, "deviceModelCode",
                            settings.GetString("model", CONFIG_DEVICE_MODEL_CODE).c_str());

    // powerSource 在第二部分契约里是 UPPER_SNAKE 字符串枚举(不是 WSS status 帧里的 int)。
    const char* power_source = (has_battery && discharging) ? "POWER_SOURCE_BATTERY"
                                                            : "POWER_SOURCE_PLUGGED";
    cJSON_AddStringToObject(root, "powerSource", power_source);
    if (has_battery) {
        cJSON_AddNumberToObject(root, "batteryLevel", battery_level);
    }

    cJSON* extra = cJSON_CreateObject();
    cJSON_AddStringToObject(extra, "boardName", BOARD_NAME);
    cJSON_AddStringToObject(extra, "mac", SystemInfo::GetMacAddress().c_str());
    cJSON_AddItemToObject(root, "extra", extra);

    std::string body = JsonToString(root);

    std::string resp;
    int status = PostRpc("ReportDeviceStatus", body, resp);
    if (status != 200) {
        return false;
    }

    // 200 但仍校验信封 code==0(拒绝响应体形如 {code:-1},只看 HTTP 状态码已足够,这里再兜一层)。
    cJSON* r = cJSON_Parse(resp.c_str());
    if (r == nullptr) {
        ESP_LOGW(TAG, "ReportDeviceStatus non-JSON response: %s", resp.c_str());
        return true;  // HTTP 200 即视为成功,data{} 可空。
    }
    auto code = cJSON_GetObjectItem(r, "code");
    bool ok = cJSON_IsNumber(code) && code->valueint == 0;
    if (!ok) {
        ESP_LOGE(TAG, "ReportDeviceStatus envelope code!=0: %s", resp.c_str());
    }
    cJSON_Delete(r);
    return ok;
}

bool CompanionHttpControl::CheckFirmwareUpdate(UpdateInfo& out) {
    auto app_desc = esp_app_get_description();

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "currentVersion", app_desc->version);
    std::string body = JsonToString(root);

    std::string resp;
    int status = PostRpc("CheckFirmwareUpdate", body, resp);
    if (status != 200) {
        return false;
    }

    cJSON* r = cJSON_Parse(resp.c_str());
    if (r == nullptr) {
        ESP_LOGE(TAG, "CheckFirmwareUpdate invalid JSON: %s", resp.c_str());
        return false;
    }

    bool ok = false;
    auto code = cJSON_GetObjectItem(r, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        ESP_LOGE(TAG, "CheckFirmwareUpdate envelope code!=0: %s", resp.c_str());
    } else {
        auto data = cJSON_GetObjectItem(r, "data");
        if (!cJSON_IsObject(data)) {
            ESP_LOGE(TAG, "CheckFirmwareUpdate missing data: %s", resp.c_str());
        } else {
            auto has_update = cJSON_GetObjectItem(data, "hasUpdate");
            out.has_update = cJSON_IsBool(has_update) ? cJSON_IsTrue(has_update) : false;

            if (out.has_update) {
                auto version = cJSON_GetObjectItem(data, "version");
                auto package_url = cJSON_GetObjectItem(data, "packageUrl");
                auto package_hash = cJSON_GetObjectItem(data, "packageHash");
                auto package_size = cJSON_GetObjectItem(data, "packageSize");
                auto force_update = cJSON_GetObjectItem(data, "forceUpdate");

                out.version = cJSON_IsString(version) ? version->valuestring : "";
                out.package_url = cJSON_IsString(package_url) ? package_url->valuestring : "";
                out.package_hash = cJSON_IsString(package_hash) ? package_hash->valuestring : "";
                // packageSize 是 int64,protojson 编码为 JSON 字符串。
                out.package_size = cJSON_IsString(package_size)
                                       ? static_cast<size_t>(strtoull(package_size->valuestring, nullptr, 10))
                                       : 0;
                out.force_update = cJSON_IsBool(force_update) ? cJSON_IsTrue(force_update) : false;
            }

            ok = true;
            ESP_LOGI(TAG, "CheckFirmwareUpdate hasUpdate=%d version=%s",
                     out.has_update, out.version.c_str());
        }
    }

    cJSON_Delete(r);
    return ok;
}
