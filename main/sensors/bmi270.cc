#include "bmi270.h"
#include "bmi270_config.h"
#include <esp_timer.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

static const char* TAG = "BMI270";

static constexpr uint8_t BMI270_CHIP_ID = 0x00;
static constexpr uint8_t BMI270_ACC_X_LSB = 0x0C;
static constexpr uint8_t BMI270_INTERNAL_STATUS = 0x21;
static constexpr uint8_t BMI270_TEMP_LSB = 0x22;
static constexpr uint8_t BMI270_ACC_CONF = 0x40;
static constexpr uint8_t BMI270_ACC_RANGE = 0x41;
static constexpr uint8_t BMI270_GYR_CONF = 0x42;
static constexpr uint8_t BMI270_GYR_RANGE = 0x43;
static constexpr uint8_t BMI270_INIT_CTRL = 0x59;
static constexpr uint8_t BMI270_INIT_ADDR_0 = 0x5B;
static constexpr uint8_t BMI270_INIT_ADDR_1 = 0x5C;
static constexpr uint8_t BMI270_INIT_DATA = 0x5E;
static constexpr uint8_t BMI270_IF_CONF = 0x6B;
static constexpr uint8_t BMI270_NV_CONF = 0x70;
static constexpr uint8_t BMI270_PWR_CONF = 0x7C;
static constexpr uint8_t BMI270_PWR_CTRL = 0x7D;
static constexpr size_t BMI270_CONFIG_CHUNK_SIZE = 32;
static constexpr int64_t BMI270_LOG_INTERVAL_MS = 1000;
static constexpr int64_t BMI270_SHAKE_COOLDOWN_MS = 1200;
static constexpr float BMI270_SHAKE_JERK_G = 0.18f;
static constexpr float BMI270_SHAKE_GYRO_DPS = 90.0f;
static constexpr float BMI270_SHAKE_SCORE = 1.0f;

BMI270::BMI270(i2c_master_bus_handle_t bus, uint8_t addr) : I2cDevice(bus, addr), addr_(addr) {
}

bool BMI270::Init() {
    uint8_t id = ReadReg(BMI270_CHIP_ID);
    ESP_LOGI(TAG, "BMI270 CHIP ID: 0x%02x", id);

    WriteReg(BMI270_PWR_CONF, 0x00);
    vTaskDelay(pdMS_TO_TICKS(2));

    WriteReg(BMI270_INIT_CTRL, 0x00);
    for (size_t offset = 0; offset < sizeof(lmcl_bmi270_config_file); offset += BMI270_CONFIG_CHUNK_SIZE) {
        uint16_t init_addr = offset / 2;
        uint8_t addr_lsb = init_addr & 0x0F;
        uint8_t addr_msb = (init_addr >> 4) & 0xFF;
        size_t chunk_size = sizeof(lmcl_bmi270_config_file) - offset;
        if (chunk_size > BMI270_CONFIG_CHUNK_SIZE) {
            chunk_size = BMI270_CONFIG_CHUNK_SIZE;
        }

        WriteReg(BMI270_INIT_ADDR_0, addr_lsb);
        WriteReg(BMI270_INIT_ADDR_1, addr_msb);
        WriteRegs(BMI270_INIT_DATA, &lmcl_bmi270_config_file[offset], chunk_size);
    }
    WriteReg(BMI270_INIT_CTRL, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t status = ReadReg(BMI270_INTERNAL_STATUS);
    if (status == 0) {
        ESP_LOGW(TAG, "BMI270 init failed, INTERNAL_STATUS=0");
        return false;
    }

    WriteReg(BMI270_PWR_CTRL, 0x0E);
    WriteReg(BMI270_NV_CONF, 0x00);
    WriteReg(BMI270_IF_CONF, 0x00);
    WriteReg(BMI270_GYR_RANGE, 0x00); // 2000 dps
    WriteReg(BMI270_GYR_CONF, 0xA9);  // 200 Hz + 3dB filter
    WriteReg(BMI270_ACC_RANGE, 0x03); // 16 g
    WriteReg(BMI270_ACC_CONF, 0xA9);  // 200 Hz + 3dB filter
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "BMI270 initialized");
    return true;
}

void BMI270::Start() {
    xTaskCreate(BMI270::Task, "bmi270_task", 4096, this, 2, NULL);
}

void BMI270::OnShake(std::function<void()> callback) {
    shake_callback_ = std::move(callback);
}

void BMI270::Task(void* arg) {
    BMI270* self = (BMI270*)arg;
    bool has_last_acc_norm = false;
    float last_acc_norm = 0.0f;
    int64_t last_log_ms = 0;
    int64_t last_shake_ms = 0;

    while (true) {
        uint8_t buffer[12] = {0};
        self->ReadRegs(BMI270_ACC_X_LSB, buffer, sizeof(buffer));

        int16_t acc_x = (int16_t)(buffer[0] | (buffer[1] << 8));
        int16_t acc_y = (int16_t)(buffer[2] | (buffer[3] << 8));
        int16_t acc_z = (int16_t)(buffer[4] | (buffer[5] << 8));
        int16_t gyr_x = (int16_t)(buffer[6] | (buffer[7] << 8));
        int16_t gyr_y = (int16_t)(buffer[8] | (buffer[9] << 8));
        int16_t gyr_z = (int16_t)(buffer[10] | (buffer[11] << 8));

        float accel_x_g = acc_x / 2048.0f;
        float accel_y_g = acc_y / 2048.0f;
        float accel_z_g = acc_z / 2048.0f;
        float gyro_x_dps = gyr_x / 16.4f;
        float gyro_y_dps = gyr_y / 16.4f;
        float gyro_z_dps = gyr_z / 16.4f;

        float acc_norm = sqrtf(accel_x_g * accel_x_g + accel_y_g * accel_y_g + accel_z_g * accel_z_g);
        float gyro_norm = sqrtf(gyro_x_dps * gyro_x_dps + gyro_y_dps * gyro_y_dps + gyro_z_dps * gyro_z_dps);
        float jerk = has_last_acc_norm ? fabsf(acc_norm - last_acc_norm) : 0.0f;
        float shake_score = jerk * 4.0f + gyro_norm / 180.0f;
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (has_last_acc_norm &&
            (now_ms - last_shake_ms) > BMI270_SHAKE_COOLDOWN_MS &&
            (jerk > BMI270_SHAKE_JERK_G || gyro_norm > BMI270_SHAKE_GYRO_DPS || shake_score > BMI270_SHAKE_SCORE)) {
            last_shake_ms = now_ms;
            ESP_LOGI(TAG, "BMI270 shake detected: jerk(g)=%.2f gyro(dps)=%.2f score=%.2f",
                     jerk, gyro_norm, shake_score);
            if (self->shake_callback_) {
                self->shake_callback_();
            }
        }

        last_acc_norm = acc_norm;
        has_last_acc_norm = true;

        if ((now_ms - last_log_ms) >= BMI270_LOG_INTERVAL_MS) {
            uint8_t temp_raw[2] = {0};
            self->ReadRegs(BMI270_TEMP_LSB, temp_raw, sizeof(temp_raw));
            int16_t temp_raw_val = (int16_t)(temp_raw[0] | (temp_raw[1] << 8));
            float temp_c = 23.0f + temp_raw_val / 512.0f;

            ESP_LOGI(TAG,
                     "BMI270 acc(g): %.2f %.2f %.2f gyro(dps): %.2f %.2f %.2f temp(C): %.2f jerk(g): %.2f score: %.2f",
                     accel_x_g, accel_y_g, accel_z_g,
                     gyro_x_dps, gyro_y_dps, gyro_z_dps,
                     temp_c, jerk, shake_score);
            last_log_ms = now_ms;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
