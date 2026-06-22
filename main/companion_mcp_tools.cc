#include "companion_mcp_tools.h"

#include "mcp_server.h"
#include "alarm_manager.h"

#include <cJSON.h>
#include <esp_log.h>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <stdexcept>

#define TAG "CompanionMcp"

namespace {
constexpr int kDefaultAlarmRingDurationSeconds = (CONFIG_ALARM_NOTIFICATION_DURATION_MS + 999) / 1000;
}

// 我方设备能力工具。当前仅闹钟一组;新增设备工具(灯/继电器等)也应集中在此注册,
// 而非焊进上游 mcp_server.cc。注册时机:application 初始化、AddCommonTools 之后。
void AddCompanionMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // ---- Alarm tools ----

    mcp.AddTool("alarm.set",
        "Set an alarm. Supports two types:\n"
        "1. Relative: fires after `delay_seconds` seconds from now (e.g. 300 = 5 minutes).\n"
        "2. Time-of-day: fires at the next occurrence of the specified `hour`:`minute` (24-hour clock).\n"
        "Parameters:\n"
        "  `type`: 'relative' or 'time_of_day'\n"
        "  `delay_seconds`: seconds from now (required when type='relative')\n"
        "  `hour`: hour 0-23 (required when type='time_of_day')\n"
        "  `minute`: minute 0-59 (required when type='time_of_day')\n"
        "  `name`: optional label for the alarm (default '闹钟')\n"
        "  `ring_duration`: how many seconds the alarm rings (default follows Alarm Settings)\n"
        "Returns: a JSON object with the new alarm's id and trigger info.",
        PropertyList({
            Property("type",         kPropertyTypeString, std::string("relative")),
            Property("delay_seconds",kPropertyTypeInteger, 60),
            Property("hour",         kPropertyTypeInteger, 0, 0, 23),
            Property("minute",       kPropertyTypeInteger, 0, 0, 59),
            Property("name",         kPropertyTypeString, std::string("闹钟")),
            Property("ring_duration",kPropertyTypeInteger, kDefaultAlarmRingDurationSeconds, 1, 300),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            auto type         = properties["type"].value<std::string>();
            auto name         = properties["name"].value<std::string>();
            int ring_duration = properties["ring_duration"].value<int>();
            int alarm_id      = -1;

            // Accept common aliases and infer type to make tool calls robust.
            std::string type_normalized = type;
            std::transform(type_normalized.begin(), type_normalized.end(), type_normalized.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (type_normalized.empty() || type_normalized == "countdown" || type_normalized == "timer") {
                type_normalized = "relative";
            } else if (type_normalized == "timeofday" || type_normalized == "time-of-day" || type_normalized == "time") {
                type_normalized = "time_of_day";
            }

            if (type_normalized == "relative") {
                int delay = properties["delay_seconds"].value<int>();
                if (delay <= 0) {
                    throw std::runtime_error("delay_seconds must be positive for relative alarms");
                }
                alarm_id = alarm_manager.AddRelativeAlarm(delay, name, ring_duration);

                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "id", alarm_id);
                cJSON_AddStringToObject(json, "name", name.c_str());
                cJSON_AddStringToObject(json, "type", "relative");
                cJSON_AddNumberToObject(json, "delay_seconds", delay);
                cJSON_AddNumberToObject(json, "ring_duration", ring_duration);
                return json;
            } else if (type_normalized == "time_of_day") {
                int hour   = properties["hour"].value<int>();
                int minute = properties["minute"].value<int>();
                alarm_id = alarm_manager.AddTimeOfDayAlarm(hour, minute, name, ring_duration);

                // Compute trigger timestamp to return to LLM
                time_t now = time(NULL);
                struct tm tm_info;
                localtime_r(&now, &tm_info);
                tm_info.tm_hour = hour;
                tm_info.tm_min  = minute;
                tm_info.tm_sec  = 0;
                time_t trigger = mktime(&tm_info);
                if (trigger <= now) trigger += 86400;

                char time_str[16];
                struct tm trig_tm;
                localtime_r(&trigger, &trig_tm);
                strftime(time_str, sizeof(time_str), "%H:%M", &trig_tm);

                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "id", alarm_id);
                cJSON_AddStringToObject(json, "name", name.c_str());
                cJSON_AddStringToObject(json, "type", "time_of_day");
                cJSON_AddStringToObject(json, "trigger_at", time_str);
                cJSON_AddNumberToObject(json, "ring_duration", ring_duration);
                return json;
            } else {
                // Fallback: if type is unrecognized, still prefer creating a relative alarm.
                int delay = properties["delay_seconds"].value<int>();
                if (delay <= 0) {
                    delay = 60;
                }
                alarm_id = alarm_manager.AddRelativeAlarm(delay, name, ring_duration);

                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "id", alarm_id);
                cJSON_AddStringToObject(json, "name", name.c_str());
                cJSON_AddStringToObject(json, "type", "relative");
                cJSON_AddNumberToObject(json, "delay_seconds", delay);
                cJSON_AddNumberToObject(json, "ring_duration", ring_duration);
                cJSON_AddBoolToObject(json, "type_fallback", true);
                return json;
            }
        });

    mcp.AddTool("alarm.cancel",
        "Cancel one or more alarms. Provide `id` to cancel a specific alarm, "
        "or `name` to cancel all alarms with that name. "
        "Use alarm.list to get the alarm ID. "
        "If id is -1 and name is empty, nothing is cancelled.",
        PropertyList({
            Property("id",   kPropertyTypeInteger, -1),
            Property("name", kPropertyTypeString,  std::string("")),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            int         id   = properties["id"].value<int>();
            std::string name = properties["name"].value<std::string>();

            if (id != -1) {
                bool ok = alarm_manager.CancelAlarm(id);
                if (!ok) {
                    throw std::runtime_error("Alarm #" + std::to_string(id) + " not found.");
                }
                return true;
            } else if (!name.empty()) {
                int count = alarm_manager.CancelAlarmsByName(name);
                if (count == 0) {
                    throw std::runtime_error("No alarm named '" + name + "' found.");
                }
                cJSON* json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "cancelled_count", count);
                return json;
            } else {
                throw std::runtime_error("Provide either 'id' or 'name' to cancel an alarm.");
            }
        });

    mcp.AddTool("alarm.cancel_all",
        "Cancel all pending and ringing alarms.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            AlarmManager::GetInstance().CancelAllAlarms();
            return true;
        });

    mcp.AddTool("alarm.list",
        "List all pending (not yet fired) alarms. Returns a JSON array of alarm objects, "
        "each with fields: id, name, trigger_time (unix timestamp), trigger_at (HH:MM string), "
        "seconds_remaining, ring_duration.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto alarms = AlarmManager::GetInstance().ListAlarms();
            time_t now  = time(NULL);
            cJSON* arr  = cJSON_CreateArray();
            for (const auto& alarm : alarms) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddNumberToObject(item, "id",          alarm.id);
                cJSON_AddStringToObject(item, "name",        alarm.name.c_str());
                cJSON_AddNumberToObject(item, "trigger_time",(double)alarm.trigger_time);

                char time_str[16];
                struct tm tm_info;
                localtime_r(&alarm.trigger_time, &tm_info);
                strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
                cJSON_AddStringToObject(item, "trigger_at",        time_str);

                long seconds_remaining = (long)(alarm.trigger_time - now);
                if (seconds_remaining < 0) seconds_remaining = 0;
                cJSON_AddNumberToObject(item, "seconds_remaining", seconds_remaining);
                cJSON_AddNumberToObject(item, "ring_duration",     alarm.ring_duration_seconds);
                cJSON_AddItemToArray(arr, item);
            }
            return arr;
        });

    mcp.AddTool("alarm.rename",
        "Rename an existing alarm. Prefer `id`; if omitted and there is exactly one pending alarm, "
        "that alarm will be renamed automatically. Works even while the alarm is ringing.",
        PropertyList({
            Property("id",       kPropertyTypeInteger, -1),
            Property("new_name", kPropertyTypeString, std::string("")),
            Property("name",     kPropertyTypeString, std::string("")),
        }),
        [](const PropertyList& properties) -> ReturnValue {
            int id               = properties["id"].value<int>();
            std::string new_name = properties["new_name"].value<std::string>();
            if (new_name.empty()) {
                new_name = properties["name"].value<std::string>();
            }

            cJSON* json = cJSON_CreateObject();
            cJSON_AddStringToObject(json, "tool", "alarm.rename");

            if (new_name.empty()) {
                cJSON_AddBoolToObject(json, "success", false);
                cJSON_AddStringToObject(json, "message", "new_name/name is empty");
                return json;
            }

            auto& alarm_manager = AlarmManager::GetInstance();
            if (id < 0) {
                id = alarm_manager.GetSingleActiveAlarmId();
                if (id < 0) {
                    cJSON_AddBoolToObject(json, "success", false);
                    cJSON_AddStringToObject(json, "message", "Cannot infer alarm id. Please specify id.");
                    return json;
                }
            }

            bool ok = alarm_manager.RenameAlarm(id, new_name);
            if (!ok) {
                cJSON_AddBoolToObject(json, "success", false);
                cJSON_AddStringToObject(json, "message", ("Alarm #" + std::to_string(id) + " not found.").c_str());
                cJSON_AddNumberToObject(json, "id", id);
                return json;
            }

            cJSON_AddBoolToObject(json, "success", true);
            cJSON_AddNumberToObject(json, "id", id);
            cJSON_AddStringToObject(json, "new_name", new_name.c_str());
            return json;
        });
}
