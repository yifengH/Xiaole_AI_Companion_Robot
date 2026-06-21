#ifndef ALARM_MANAGER_H
#define ALARM_MANAGER_H

#include <string>
#include <vector>
#include <mutex>
#include <ctime>
#include <esp_timer.h>

struct AlarmInfo {
    int id;
    std::string name;
    time_t trigger_time;
    int ring_duration_seconds;
    bool active;
};

class AlarmManager {
public:
    static AlarmManager& GetInstance();

    AlarmManager(const AlarmManager&) = delete;
    AlarmManager& operator=(const AlarmManager&) = delete;

    // Set a relative alarm (fires after delay_seconds seconds from now)
    int AddRelativeAlarm(int delay_seconds, const std::string& name, int ring_duration);

    // Set a time-of-day alarm (fires at next occurrence of hour:minute)
    int AddTimeOfDayAlarm(int hour, int minute, const std::string& name, int ring_duration);

    // Cancel alarm by ID (also stops ringing if active). Returns true if found.
    bool CancelAlarm(int id);

    // Cancel all alarms whose name matches. Returns count of cancelled alarms.
    int CancelAlarmsByName(const std::string& name);

    // Rename alarm by ID (also renames if currently ringing). Returns true if found.
    bool RenameAlarm(int id, const std::string& new_name);

    // If exactly one alarm exists across pending/ringing, return its id; otherwise return -1.
    int GetSingleActiveAlarmId() const;

    // Cancel all pending and ringing alarms.
    void CancelAllAlarms();

    // Get list of pending (not yet fired) alarms.
    std::vector<AlarmInfo> ListAlarms() const;

    // Whether there are alarms currently in ringing period.
    bool HasRingingAlarms() const;

private:
    AlarmManager();
    ~AlarmManager();

    void OnTimerTick();

    mutable std::mutex mutex_;
    std::vector<AlarmInfo> alarms_;
    int next_id_ = 1;
    esp_timer_handle_t timer_ = nullptr;

    struct RingingState {
        int alarm_id;
        std::string name;
        time_t ring_end_time;
        time_t next_play_time;
        bool first_play;
    };
    std::vector<RingingState> ringing_states_;
};

#endif // ALARM_MANAGER_H
