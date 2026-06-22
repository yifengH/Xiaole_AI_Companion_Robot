#include "ota.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_app_desc.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <mbedtls/sha256.h>

#include <cstring>
#include <cstdio>
#include <string>

#define TAG "Ota"

namespace {

// 把 32 字节 sha256 摘要转成小写 hex 字符串(长度 64),与服务端 packageHash 比对。
std::string ToHex(const uint8_t digest[32]) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; i++) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

} // namespace

std::string Ota::GetCurrentVersion() {
    auto app_desc = esp_app_get_description();
    return app_desc->version;
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url,
                  const std::string& expected_sha256_hex,
                  size_t expected_size,
                  std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s (expected sha256=%s size=%u)",
        firmware_url.c_str(), expected_sha256_hex.c_str(), (unsigned)expected_size);
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    // 边下边算 sha256,下完与服务端 packageHash 比对。
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0);

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    // packageUrl 是 OSS 直链,直接 GET,无需令牌。
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }
    // 大小护栏:服务端给了 packageSize 就先核对 Content-Length,不符直接拒(省得白下一整包)。
    if (expected_size > 0 && content_length != expected_size) {
        ESP_LOGE(TAG, "Content length %u != expected packageSize %u, abort",
            (unsigned)content_length, (unsigned)expected_size);
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        mbedtls_sha256_free(&sha_ctx);
        return false;
    }

    size_t buffer_offset = 0;  // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    while (true) {
        int ret = http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset);
        if (ret < 0) {
            ESP_LOGE(TAG, "Failed to read HTTP data: %s", esp_err_to_name(ret));
            mbedtls_sha256_free(&sha_ctx);
            heap_caps_free(buffer);
            if (image_header_checked) {
                esp_ota_abort(update_handle);
            }
            return false;
        }

        // 新读到的 ret 字节计入 sha256(覆盖整包 firmware 字节)。
        if (ret > 0) {
            mbedtls_sha256_update(&sha_ctx, (const uint8_t*)(buffer + buffer_offset), ret);
        }

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000 || ret == 0) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    mbedtls_sha256_free(&sha_ctx);
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB) or it's the last chunk
        bool is_last_chunk = (ret == 0);
        if (buffer_offset == PAGE_SIZE || (is_last_chunk && buffer_offset > 0)) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                mbedtls_sha256_free(&sha_ctx);
                heap_caps_free(buffer);
                return false;
            }

            buffer_offset = 0;
        }

        if (is_last_chunk) {
            break;
        }
    }
    http->Close();
    heap_caps_free(buffer);

    // —— 校验:大小 + sha256;任一不符即丢弃不装(契约硬性要求)——
    uint8_t digest[32];
    mbedtls_sha256_finish(&sha_ctx, digest);
    mbedtls_sha256_free(&sha_ctx);

    if (expected_size > 0 && total_read != expected_size) {
        ESP_LOGE(TAG, "Downloaded size %u != expected packageSize %u, discard", (unsigned)total_read, (unsigned)expected_size);
        esp_ota_abort(update_handle);
        return false;
    }
    if (!expected_sha256_hex.empty()) {
        std::string actual = ToHex(digest);
        // 大小写不敏感比对(服务端发小写 hex,稳妥起见仍比对小写)。
        if (actual != expected_sha256_hex) {
            ESP_LOGE(TAG, "sha256 mismatch: got %s want %s, discard", actual.c_str(), expected_sha256_hex.c_str());
            esp_ota_abort(update_handle);
            return false;
        }
        ESP_LOGI(TAG, "sha256 verified: %s", actual.c_str());
    }

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}
