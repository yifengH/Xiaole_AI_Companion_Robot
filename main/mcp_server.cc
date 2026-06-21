/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <esp_pthread.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "alarm_manager.h"

#define TAG "MCP"

namespace {
constexpr int kDefaultAlarmRingDurationSeconds = (CONFIG_ALARM_NOTIFICATION_DURATION_MS + 999) / 1000;
}

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Always remember you have a camera. If the user asks you to see something, use this tool to take a photo and then explain it.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
#endif

    // ---- Alarm tools ----

    AddTool("alarm.set",
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

    AddTool("alarm.cancel",
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

    AddTool("alarm.cancel_all",
        "Cancel all pending and ringing alarms.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            AlarmManager::GetInstance().CancelAllAlarms();
            return true;
        });

    AddTool("alarm.list",
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

    AddTool("alarm.rename",
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

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                bool success = app.UpgradeFirmware(url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
