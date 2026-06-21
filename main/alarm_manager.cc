/*
 * Alarm Manager
 * Manages multiple named alarms, supports both relative (countdown) and
 * time-of-day alarms. Plays assets/common/alarm_ring.ogg when triggered.
 */

#include "alarm_manager.h"
#include "application.h"
#include "board.h"
#include "display.h"

#include <esp_log.h>
#include <font_awesome.h>
#include <string_view>
#include <algorithm>
#include <cstring>

#define TAG "Alarm"

// Alarm tuning – adjust via idf.py menuconfig → "Alarm Settings"
#define ALARM_RING_INTERVAL_SECONDS     CONFIG_ALARM_RING_INTERVAL_SECONDS
#define ALARM_GAIN_PERCENT              CONFIG_ALARM_GAIN_PERCENT
#define DEFAULT_ALARM_RING_DURATION_SECONDS ((CONFIG_ALARM_NOTIFICATION_DURATION_MS + 999) / 1000)

// Reference to embedded binary for alarm_ring.ogg (from assets/common/)
extern const char ogg_alarm_ring_start[] asm("_binary_alarm_ring_ogg_start");
extern const char ogg_alarm_ring_end[]   asm("_binary_alarm_ring_ogg_end");

static const std::string_view OGG_ALARM_RING{
    static_cast<const char*>(ogg_alarm_ring_start),
    static_cast<size_t>(ogg_alarm_ring_end - ogg_alarm_ring_start)
};

namespace {
bool IsVoiceInteractionActive(DeviceState state, bool voice_detected) {
    // Treat active conversation as "interaction":
    // - connecting / speaking are always interactive
    // - listening is interactive only while local VAD detects speech
    return state == kDeviceStateConnecting ||
           state == kDeviceStateSpeaking ||
           (state == kDeviceStateListening && voice_detected);
}
}

// ---------------------------------------------------------------------------

AlarmManager& AlarmManager::GetInstance() {
    static AlarmManager instance;
    return instance;
}

AlarmManager::AlarmManager() {
    esp_timer_create_args_t args = {
        .callback = [](void* arg) {
            static_cast<AlarmManager*>(arg)->OnTimerTick();
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "alarm_check",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &timer_);
    esp_timer_start_periodic(timer_, 1000000ULL); // 1 s
}

AlarmManager::~AlarmManager() {
    if (timer_) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Public API

int AlarmManager::AddRelativeAlarm(int delay_seconds, const std::string& name, int ring_duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    AlarmInfo alarm;
    alarm.id = next_id_++;
    alarm.name = name.empty() ? "闹钟" : name;
    alarm.trigger_time = time(NULL) + delay_seconds;
    alarm.ring_duration_seconds = ring_duration > 0 ? ring_duration : DEFAULT_ALARM_RING_DURATION_SECONDS;
    alarm.active = true;
    alarms_.push_back(alarm);
    ESP_LOGI(TAG, "Set relative alarm #%d '%s' in %ds, ring %ds",
             alarm.id, alarm.name.c_str(), delay_seconds, alarm.ring_duration_seconds);
    return alarm.id;
}

int AlarmManager::AddTimeOfDayAlarm(int hour, int minute, const std::string& name, int ring_duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = hour;
    tm_info.tm_min  = minute;
    tm_info.tm_sec  = 0;
    time_t trigger = mktime(&tm_info);
    if (trigger <= now) {
        trigger += 86400; // Schedule for tomorrow if time already passed today
    }
    AlarmInfo alarm;
    alarm.id = next_id_++;
    alarm.name = name.empty() ? "闹钟" : name;
    alarm.trigger_time = trigger;
    alarm.ring_duration_seconds = ring_duration > 0 ? ring_duration : DEFAULT_ALARM_RING_DURATION_SECONDS;
    alarm.active = true;
    alarms_.push_back(alarm);
    ESP_LOGI(TAG, "Set time-of-day alarm #%d '%s' at %02d:%02d, ring %ds",
             alarm.id, alarm.name.c_str(), hour, minute, alarm.ring_duration_seconds);
    return alarm.id;
}

bool AlarmManager::CancelAlarm(int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto& alarm : alarms_) {
        if (alarm.id == id && alarm.active) {
            alarm.active = false;
            found = true;
            ESP_LOGI(TAG, "Cancelled alarm #%d '%s'", id, alarm.name.c_str());
        }
    }
    for (auto& rs : ringing_states_) {
        if (rs.alarm_id == id) {
            rs.ring_end_time = 0; // Force stop ringing immediately
            found = true;
        }
    }
    return found;
}

int AlarmManager::CancelAlarmsByName(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (auto& alarm : alarms_) {
        if (alarm.name == name && alarm.active) {
            alarm.active = false;
            count++;
        }
    }
    for (auto& rs : ringing_states_) {
        if (rs.name == name) {
            rs.ring_end_time = 0;
            count++;
        }
    }
    return count;
}

bool AlarmManager::RenameAlarm(int id, const std::string& new_name) {
    if (new_name.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto& alarm : alarms_) {
        if (alarm.id == id && alarm.active) {
            alarm.name = new_name;
            found = true;
        }
    }
    for (auto& rs : ringing_states_) {
        if (rs.alarm_id == id) {
            rs.name = new_name;
            found = true;
        }
    }
    if (found) {
        ESP_LOGI(TAG, "Renamed alarm #%d to '%s'", id, new_name.c_str());
    }
    return found;
}

int AlarmManager::GetSingleActiveAlarmId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int found_id = -1;
    int count = 0;

    for (const auto& alarm : alarms_) {
        if (!alarm.active) {
            continue;
        }
        found_id = alarm.id;
        count++;
        if (count > 1) {
            return -1;
        }
    }

    for (const auto& rs : ringing_states_) {
        if (found_id == rs.alarm_id) {
            continue;
        }
        found_id = rs.alarm_id;
        count++;
        if (count > 1) {
            return -1;
        }
    }

    return count == 1 ? found_id : -1;
}

void AlarmManager::CancelAllAlarms() {
    std::lock_guard<std::mutex> lock(mutex_);
    alarms_.clear();
    for (auto& rs : ringing_states_) {
        rs.ring_end_time = 0;
    }
}

std::vector<AlarmInfo> AlarmManager::ListAlarms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<AlarmInfo> result;
    for (const auto& alarm : alarms_) {
        if (alarm.active) {
            result.push_back(alarm);
        }
    }
    return result;
}

bool AlarmManager::HasRingingAlarms() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !ringing_states_.empty();
}

// ---------------------------------------------------------------------------
// Private timer callback (runs every 1 second in ESP_TIMER_TASK context)

void AlarmManager::OnTimerTick() {
    time_t now = time(NULL);
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    const bool interaction_active = IsVoiceInteractionActive(state, app.IsVoiceDetected());

    int play_count = 0;
    bool should_show_overlay = false;
    std::string overlay_name;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check pending alarms
        for (auto& alarm : alarms_) {
            if (alarm.active && now >= alarm.trigger_time) {
                alarm.active = false;
                ESP_LOGI(TAG, "Alarm #%d '%s' triggered!", alarm.id, alarm.name.c_str());
                RingingState rs;
                rs.alarm_id       = alarm.id;
                rs.name           = alarm.name;
                rs.ring_end_time  = now + alarm.ring_duration_seconds;
                rs.next_play_time = now; // Play immediately
                rs.first_play     = true;
                ringing_states_.push_back(std::move(rs));
            }
        }

        // Remove fired alarms from the pending list
        alarms_.erase(
            std::remove_if(alarms_.begin(), alarms_.end(),
                           [](const AlarmInfo& a) { return !a.active; }),
            alarms_.end());

        if (interaction_active) {
            // Freeze ringing lifetime while user is actively interacting so
            // the alarm can resume after interaction ends.
            for (auto& rs : ringing_states_) {
                if (now < rs.ring_end_time && !rs.first_play) {
                    rs.ring_end_time += 1;
                }
            }
        }

        // Determine which ringing alarms need a sound play this tick
        for (auto& rs : ringing_states_) {
            if (now < rs.ring_end_time && now >= rs.next_play_time) {
                if (interaction_active && !rs.first_play) {
                    rs.next_play_time = now;
                    continue;
                }
                ++play_count;
                rs.first_play     = false;
                rs.next_play_time = now + ALARM_RING_INTERVAL_SECONDS;
            }
        }

        // Remove expired ringing sessions
        ringing_states_.erase(
            std::remove_if(ringing_states_.begin(), ringing_states_.end(),
                           [now](const RingingState& rs) { return now >= rs.ring_end_time; }),
            ringing_states_.end());

        if (!ringing_states_.empty()) {
            // Show overlay continuously while at least one alarm is ringing.
            should_show_overlay = true;
            overlay_name = ringing_states_.front().name;
        }
    }

    // Keep alarm overlay/icon state in sync once per second.
    Application::GetInstance().Schedule([should_show_overlay, overlay_name = std::move(overlay_name)]() {
        auto display = Board::GetInstance().GetDisplay();
        if (!display) {
            return;
        }

        if (should_show_overlay) {
            std::string msg = std::string(FONT_AWESOME_ALARM_CLOCK) + " 闹钟提示\n" + overlay_name + "\n";
            display->ShowAlarmOverlay(msg.c_str(), 0);
        } else {
            display->HideAlarmOverlay();
        }

        display->UpdateStatusBar();
    });

    // Schedule sound playback in the main task (outside lock)
    for (int i = 0; i < play_count; ++i) {
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();

            // wait_if_busy=true avoids partial/truncated ring caused by packet dropping
            // when decode queue is temporarily busy. Mark alarm playback as ducking-
            // eligible so active voice interaction can suppress the local ring enough
            // for AEC/STT/MCP cancel commands to work while the alarm is still active.
            app.GetAudioService().PlaySound(OGG_ALARM_RING, true, true, ALARM_GAIN_PERCENT);
        });
    }
}
