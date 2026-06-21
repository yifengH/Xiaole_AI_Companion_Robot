#include <esp_log.h>
#include <esp_err.h>
#include <string>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <font_awesome.h>

#include "lvgl_display.h"
#include "board.h"
#include "application.h"
#include "audio_codec.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "jpg/image_to_jpeg.h"

#define TAG "Display"

LvglDisplay::LvglDisplay() {
    // Notification timer
    esp_timer_create_args_t notification_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            lv_obj_add_flag(display->notification_label_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(display->status_label_, LV_OBJ_FLAG_HIDDEN);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "notification_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&notification_timer_args, &notification_timer_));

    esp_timer_create_args_t alarm_overlay_timer_args = {
        .callback = [](void *arg) {
            LvglDisplay *display = static_cast<LvglDisplay*>(arg);
            DisplayLockGuard lock(display);
            if (display->alarm_overlay_ != nullptr && lv_obj_is_valid(display->alarm_overlay_)) {
                lv_obj_add_flag(display->alarm_overlay_, LV_OBJ_FLAG_HIDDEN);
                display->alarm_overlay_visible_ = false;
            }
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_overlay_timer",
        .skip_unhandled_events = false,
    };
    ESP_ERROR_CHECK(esp_timer_create(&alarm_overlay_timer_args, &alarm_overlay_timer_));

    // Create a power management lock
    auto ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "display_update", &pm_lock_);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "Power management not supported");
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

LvglDisplay::~LvglDisplay() {
    if (notification_timer_ != nullptr) {
        esp_timer_stop(notification_timer_);
        esp_timer_delete(notification_timer_);
    }
    if (alarm_overlay_timer_ != nullptr) {
        esp_timer_stop(alarm_overlay_timer_);
        esp_timer_delete(alarm_overlay_timer_);
    }

    if (alarm_overlay_ != nullptr && lv_obj_is_valid(alarm_overlay_)) {
        lv_obj_del(alarm_overlay_);
        alarm_overlay_ = nullptr;
        alarm_overlay_label_ = nullptr;
    }

    if (network_label_ != nullptr) {
        lv_obj_del(network_label_);
    }
    if (notification_label_ != nullptr) {
        lv_obj_del(notification_label_);
    }
    if (status_label_ != nullptr) {
        lv_obj_del(status_label_);
    }
    if (mute_label_ != nullptr) {
        lv_obj_del(mute_label_);
    }
    if (battery_label_ != nullptr) {
        lv_obj_del(battery_label_);
    }
    if( low_battery_popup_ != nullptr ) {
        lv_obj_del(low_battery_popup_);
    }
    if (pm_lock_ != nullptr) {
        esp_pm_lock_delete(pm_lock_);
    }
}

void LvglDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI() - message will be lost!", status);
    }
    DisplayLockGuard lock(this);
    if (status_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetStatus('%s') failed: status_label_ is nullptr (SetupUI() was called but label not created)", status);
        }
        return;
    }
    lv_label_set_text(status_label_, status);
    lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    last_status_update_time_ = std::chrono::system_clock::now();
}

void LvglDisplay::ShowNotification(const std::string &notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void LvglDisplay::ShowNotification(const char* notification, int duration_ms) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "ShowNotification('%s') called before SetupUI() - message will be lost!", notification);
    }
    DisplayLockGuard lock(this);
    if (notification_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "ShowNotification('%s') failed: notification_label_ is nullptr (SetupUI() was called but label not created)", notification);
        }
        return;
    }
    lv_label_set_text(notification_label_, notification);
    lv_obj_remove_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);

    esp_timer_stop(notification_timer_);
    ESP_ERROR_CHECK(esp_timer_start_once(notification_timer_, duration_ms * 1000));
}

void LvglDisplay::EnsureAlarmOverlay() {
    if (alarm_overlay_ != nullptr && !lv_obj_is_valid(alarm_overlay_)) {
        alarm_overlay_ = nullptr;
        alarm_overlay_label_ = nullptr;
    }

    if (alarm_overlay_ != nullptr) {
        return;
    }

    alarm_overlay_ = lv_obj_create(lv_layer_top());
    lv_obj_clear_flag(alarm_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(alarm_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(alarm_overlay_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_scrollbar_mode(alarm_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(alarm_overlay_, LV_OPA_40, 0);
    lv_obj_set_style_bg_color(alarm_overlay_, lv_color_hex(0x101820), 0);
    lv_obj_set_style_border_width(alarm_overlay_, 0, 0);
    lv_obj_set_style_radius(alarm_overlay_, 16, 0);
    lv_obj_set_style_pad_all(alarm_overlay_, 12, 0);
    lv_obj_set_style_shadow_width(alarm_overlay_, 0, 0);

    alarm_overlay_label_ = lv_label_create(alarm_overlay_);
    lv_obj_set_style_text_color(alarm_overlay_label_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(alarm_overlay_label_, LV_TEXT_ALIGN_CENTER, 0);
    if (status_label_ != nullptr) {
        const lv_font_t* status_font = (const lv_font_t*)lv_obj_get_style_text_font(status_label_, LV_PART_MAIN);
        if (status_font != nullptr) {
            lv_obj_set_style_text_font(alarm_overlay_label_, status_font, 0);
        }
    }
    lv_label_set_long_mode(alarm_overlay_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(alarm_overlay_label_, "");

    lv_obj_add_flag(alarm_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void LvglDisplay::UpdateAlarmOverlayLayout() {
    if (alarm_overlay_ == nullptr || alarm_overlay_label_ == nullptr) {
        return;
    }

    const int32_t screen_w = lv_display_get_horizontal_resolution(display_);
    const int32_t screen_h = lv_display_get_vertical_resolution(display_);
    if (screen_w <= 0 || screen_h <= 0) {
        return;
    }

    constexpr float kAspectRatio = 3.0f;
    const bool portrait = screen_h >= screen_w;
    int32_t box_w = 0;
    int32_t box_h = 0;

    if (portrait) {
        box_w = static_cast<int32_t>(screen_w * 0.85f);
        box_h = static_cast<int32_t>(box_w / kAspectRatio);
    } else {
        box_h = static_cast<int32_t>(screen_h * 0.85f);
        box_w = static_cast<int32_t>(box_h * kAspectRatio);
    }

    const int32_t max_w = static_cast<int32_t>(screen_w * 0.95f);
    const int32_t max_h = static_cast<int32_t>(screen_h * 0.95f);
    if (box_w > max_w) {
        box_w = max_w;
        box_h = static_cast<int32_t>(box_w / kAspectRatio);
    }
    if (box_h > max_h) {
        box_h = max_h;
        box_w = static_cast<int32_t>(box_h * kAspectRatio);
    }

    box_w = std::max<int32_t>(120, box_w);
    box_h = std::max<int32_t>(48, box_h);

    lv_obj_set_size(alarm_overlay_, box_w, box_h);
    lv_obj_center(alarm_overlay_);
    lv_obj_set_width(alarm_overlay_label_, box_w - 24);
    lv_obj_center(alarm_overlay_label_);
    lv_obj_move_foreground(alarm_overlay_);
}

void LvglDisplay::ShowAlarmOverlay(const char* message, int duration_ms) {
    if (!setup_ui_called_) {
        return;
    }

    DisplayLockGuard lock(this);
    EnsureAlarmOverlay();
    if (alarm_overlay_ == nullptr || alarm_overlay_label_ == nullptr) {
        return;
    }

    UpdateAlarmOverlayLayout();
    lv_label_set_text(alarm_overlay_label_, message);
    lv_obj_remove_flag(alarm_overlay_, LV_OBJ_FLAG_HIDDEN);
    alarm_overlay_visible_ = true;

    esp_timer_stop(alarm_overlay_timer_);
    if (duration_ms > 0) {
        ESP_ERROR_CHECK(esp_timer_start_once(alarm_overlay_timer_, static_cast<uint64_t>(duration_ms) * 1000ULL));
    }
}

void LvglDisplay::HideAlarmOverlay() {
    DisplayLockGuard lock(this);
    if (alarm_overlay_ != nullptr && lv_obj_is_valid(alarm_overlay_)) {
        lv_obj_add_flag(alarm_overlay_, LV_OBJ_FLAG_HIDDEN);
    }
    alarm_overlay_visible_ = false;
    esp_timer_stop(alarm_overlay_timer_);
}

void LvglDisplay::UpdateStatusBar(bool update_all) {
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto codec = board.GetAudioCodec();

    // Update alarm icon
    {
        DisplayLockGuard lock(this);
        if (alarm_label_ != nullptr) {
            const bool ringing = alarm_overlay_visible_;
            if (ringing != alarm_icon_visible_) {
                alarm_icon_visible_ = ringing;
                lv_label_set_text(alarm_label_, ringing ? FONT_AWESOME_ALARM_CLOCK : "");
            }
        }
    }

    // Update mute icon
    {
        DisplayLockGuard lock(this);
        if (mute_label_ == nullptr) {
            return;
        }

        // Update icon if mute state changes
        if (codec->output_volume() == 0 && !muted_) {
            muted_ = true;
            lv_label_set_text(mute_label_, FONT_AWESOME_VOLUME_XMARK);
        } else if (codec->output_volume() > 0 && muted_) {
            muted_ = false;
            lv_label_set_text(mute_label_, "");
        }
    }

    // Update time
    if (app.GetDeviceState() == kDeviceStateIdle) {
        if (last_status_update_time_ + std::chrono::seconds(10) < std::chrono::system_clock::now()) {
            // Set status to clock "HH:MM"
            time_t now = time(NULL);
            struct tm* tm = localtime(&now);
            // Check if the we have already set the time
            if (tm->tm_year >= 2025 - 1900) {
                char time_str[16];
                strftime(time_str, sizeof(time_str), "%H:%M", tm);
                SetStatus(time_str);
            } else {
                ESP_LOGW(TAG, "System time is not set, tm_year: %d", tm->tm_year);
            }
        }
    }

    esp_pm_lock_acquire(pm_lock_);
    // Update battery icon
    int battery_level;
    bool charging, discharging;
    const char* icon = nullptr;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        if (charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else {
            const char* levels[] = {
                FONT_AWESOME_BATTERY_EMPTY, // 0-19%
                FONT_AWESOME_BATTERY_QUARTER,    // 20-39%
                FONT_AWESOME_BATTERY_HALF,    // 40-59%
                FONT_AWESOME_BATTERY_THREE_QUARTERS,    // 60-79%
                FONT_AWESOME_BATTERY_FULL, // 80-99%
                FONT_AWESOME_BATTERY_FULL, // 100%
            };
            icon = levels[battery_level / 20];
        }
        DisplayLockGuard lock(this);
        if (battery_label_ != nullptr && battery_icon_ != icon) {
            battery_icon_ = icon;
            lv_label_set_text(battery_label_, battery_icon_);
        }

        // Check low battery popup only when clock tick event is triggered
        // Because when initializing, the battery level is not ready yet.
        if (low_battery_popup_ != nullptr && !update_all) {
            if (strcmp(icon, FONT_AWESOME_BATTERY_EMPTY) == 0 && discharging) {
                if (lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Show if low battery popup is hidden
                    lv_obj_remove_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                    app.Schedule([&app]() {
                        app.PlaySound(Lang::Sounds::OGG_LOW_BATTERY);
                    });
                }
            } else {
                // Hide the low battery popup when the battery is not empty
                if (!lv_obj_has_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN)) { // Hide if low battery popup is shown
                    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    // Update network icon every 10 seconds
    static int seconds_counter = 0;
    if (update_all || seconds_counter++ % 10 == 0) {
        // Don't read 4G network status during firmware upgrade to avoid occupying UART resources
        auto device_state = Application::GetInstance().GetDeviceState();
        static const std::vector<DeviceState> allowed_states = {
            kDeviceStateIdle,
            kDeviceStateStarting,
            kDeviceStateWifiConfiguring,
            kDeviceStateListening,
            kDeviceStateActivating,
        };
        if (std::find(allowed_states.begin(), allowed_states.end(), device_state) != allowed_states.end()) {
            icon = board.GetNetworkStateIcon();
            if (network_label_ != nullptr && icon != nullptr && network_icon_ != icon) {
                DisplayLockGuard lock(this);
                network_icon_ = icon;
                lv_label_set_text(network_label_, network_icon_);
            }
        }
    }

    esp_pm_lock_release(pm_lock_);
}

void LvglDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
}

void LvglDisplay::SetPowerSaveMode(bool on) {
    if (on) {
        SetChatMessage("system", "");
        SetEmotion("sleepy");
    } else {
        SetChatMessage("system", "");
        SetEmotion("neutral");
    }
}

bool LvglDisplay::SnapshotToJpeg(std::string& jpeg_data, int quality) {
#if CONFIG_LV_USE_SNAPSHOT
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_screen_active();
    lv_draw_buf_t* draw_buffer = lv_snapshot_take(screen, LV_COLOR_FORMAT_RGB565);
    if (draw_buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to take snapshot, draw_buffer is nullptr");
        return false;
    }

    // swap bytes
    uint16_t* data = (uint16_t*)draw_buffer->data;
    size_t pixel_count = draw_buffer->data_size / 2;
    for (size_t i = 0; i < pixel_count; i++) {
        data[i] = __builtin_bswap16(data[i]);
    }

    // Clear output string and use callback version to avoid pre-allocating large memory blocks
    jpeg_data.clear();

    // Use callback-based JPEG encoder to further save memory
    bool ret = image_to_jpeg_cb((uint8_t*)draw_buffer->data, draw_buffer->data_size, draw_buffer->header.w, draw_buffer->header.h, V4L2_PIX_FMT_RGB565, quality,
        [](void *arg, size_t index, const void *data, size_t len) -> size_t {
        std::string* output = static_cast<std::string*>(arg);
        if (data && len > 0) {
            output->append(static_cast<const char*>(data), len);
        }
        return len;
    }, &jpeg_data);
    if (!ret) {
        ESP_LOGE(TAG, "Failed to convert image to JPEG");
    }

    lv_draw_buf_destroy(draw_buffer);
    return ret;
#else
    ESP_LOGE(TAG, "LV_USE_SNAPSHOT is not enabled");
    return false;
#endif
}
