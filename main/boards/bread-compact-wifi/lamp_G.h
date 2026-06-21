/* 我已经将conpact_wifi_board.cc中系统默认的LED控制行注释了，小智内置开关灯为18号针脚，
   所以可以直接用18号针脚的话，就要将上面的注释恢复；
   如果想用17号针脚，就不要恢复注释了。
    同时现在增加了LED灯亮后，，10秒自动关闭的功能，以及闪烁功能，闪烁功能会让灯闪烁10次，每次闪烁持续1秒钟（500ms开，500ms关）。
*/
#include "mcp_server.h"
#include <esp_log.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LED_LAMP_TAG "LED灯"

class LED_Lamp  {          // 1. 类名改成 LED_Lamp 
private:
    bool power_ = false;
    gpio_num_t gpio_num_;

public: //如果修改类名LED_Lamp，下面一行中的LED_Lamp也要同时修改
    explicit LED_Lamp (gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num_, 0);       //初始化为低电平

        /* 2. 把 MCP 工具名改成LED灯相关 */
        
        auto& server = McpServer::GetInstance();
        server.AddTool("LED灯.获取开关状态", "返回LED灯的开/关状态",      // 工具名称   , 工具描述    
                       PropertyList(), [this](const PropertyList&) {

                           ESP_LOGW(LED_LAMP_TAG, "获取到了LED灯的当前状态，当前状态为%s", power_ ? "开" : "关");     //日志记录
                           return power_ ? "{\"灯光状态：\":LED灯是开着的！}" : "{\"灯光状态：\":LED灯是关着的！}";       //返回状态
                       
                       });
   
        server.AddTool("LED灯.打开", "打开LED灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = true;
                           gpio_set_level(gpio_num_, 1);    //设置为高电平
                           ESP_LOGW(LED_LAMP_TAG, "已打开LED灯！");   //日志记录
                           
                           // 创建一个任务，5秒后自动关闭LED灯
                           LED_Lamp* lamp_instance = this;
                           xTaskCreate([](void* param) {
                               LED_Lamp* instance = static_cast<LED_Lamp*>(param);
                               vTaskDelay(pdMS_TO_TICKS(10000)); // 延时10秒
                               if (instance->power_) { // 确保灯还是开着的
                                   instance->power_ = false;
                                   gpio_set_level(instance->gpio_num_, 0);
                                   ESP_LOGW(LED_LAMP_TAG, "LED灯已自动关闭！");
                               }
                               vTaskDelete(NULL);
                           }, "auto_off_task", 2048, lamp_instance, 5, NULL);
                           
                           return true;     //返回告诉小智执行成功！
                       });

        server.AddTool("LED灯.关闭", "关闭LED灯",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = false;
                           gpio_set_level(gpio_num_, 0);    //设置为低电平
                           ESP_LOGW(LED_LAMP_TAG, "已关闭LED灯！");    //日志记录
                           return true;     //返回告诉小智执行成功！
                       });

        server.AddTool("LED灯.闪烁", "让LED灯闪烁",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           ESP_LOGW(LED_LAMP_TAG, "LED灯开始闪烁！");   //日志记录
                           
                           // 创建一个任务，让LED灯闪烁10次
                           LED_Lamp* blink_instance = this;
                           xTaskCreate([](void* param) {
                               LED_Lamp* instance = static_cast<LED_Lamp*>(param);
                               for (int i = 0; i < 10; i++) {
                                   gpio_set_level(instance->gpio_num_, 1);    // 打开LED
                                   vTaskDelay(pdMS_TO_TICKS(500));  // 延时500ms
                                   gpio_set_level(instance->gpio_num_, 0);    // 关闭LED
                                   vTaskDelay(pdMS_TO_TICKS(500));  // 延时500ms
                               }
                               // 闪烁结束后保持关闭状态
                               instance->power_ = false;
                               ESP_LOGW(LED_LAMP_TAG, "LED灯闪烁结束！");
                               vTaskDelete(NULL);
                           }, "blink_task", 2048, blink_instance, 5, NULL);
                           
                           return true;     //返回告诉小智执行成功！
                       });
    }
};