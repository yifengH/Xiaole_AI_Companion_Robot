// 本代码中继电器控制规划：打开为吸合，关闭为断开

#include "mcp_server.h"
#include <esp_log.h>

#define JIDIAN_TAG "继电器"

class jidian_Lamp  {          // 1. 类名改成 jidian_Lamp 
private:
    bool power_ = false;
    gpio_num_t gpio_num_;

public:
    explicit jidian_Lamp (gpio_num_t gpio_num) : gpio_num_(gpio_num) {
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << gpio_num_),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        gpio_set_level(gpio_num_, 0);       //初始化为低电平

        /* 2. 把 MCP 工具名改成继电器相关 */
        
        auto& server = McpServer::GetInstance();
        server.AddTool("继电器.获取开关状态", "返回继电器的开（吸合）/关（断开）状态",      // 工具名称   , 工具描述    
                       PropertyList(), [this](const PropertyList&) {

                           ESP_LOGW(JIDIAN_TAG, "获取到了继电器的当前状态，当前状态为%s", power_ ? "开" : "关");     //日志记录
                           return power_ ? "{\"继电器状态：\":继电器是开着的！}" : "{\"继电器状态：\":继电器是关着的！}";       //返回状态
                       
                       });
   
        server.AddTool("继电器.打开", "打开继电器",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = true;
                           gpio_set_level(gpio_num_, 1);    //设置为高电平（继电器吸合）
                           ESP_LOGW(JIDIAN_TAG, "已打开继电器！");   //日志记录
                           return true;     //返回告诉小智执行成功！
                       });

        server.AddTool("继电器.关闭", "关闭继电器",     // 工具名称   , 工具描述
                       PropertyList(), [this](const PropertyList&) {
                           power_ = false;
                           gpio_set_level(gpio_num_, 0);    //设置为低电平（继电器断开）
                           ESP_LOGW(JIDIAN_TAG, "已关闭继电器！");    //日志记录
                           return true;     //返回告诉小智执行成功！
                       });
    }
};
