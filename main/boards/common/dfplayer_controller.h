#ifndef __DFPLAYER_CONTROLLER_H__
#define __DFPLAYER_CONTROLLER_H__

#include "mcp_server.h"
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char* DFPLAYER_TAG = "DFPlayer";

class DfplayerController {
private:
    uart_port_t uart_num_;
    int volume_ = 15;  // DFPlayer 默认音量为15，范围0-30

    void SendCommand(uint8_t cmd, uint16_t data = 0) {
        uint8_t buf[10];
        buf[0] = 0x7E;  // start
        buf[1] = 0xFF;  // version
        buf[2] = 0x06;  // length
        buf[3] = cmd;
        buf[4] = 0x00;  // no feedback
        buf[5] = (data >> 8) & 0xFF;  // data high
        buf[6] = data & 0xFF;         // data low
        uint16_t sum = 0xFFFF - (0xFF + 0x06 + cmd + 0x00 + buf[5] + buf[6]) + 1;
        buf[7] = (sum >> 8) & 0xFF;
        buf[8] = sum & 0xFF;
        buf[9] = 0xEF;  // end

        uart_write_bytes(uart_num_, buf, 10);
        uart_flush(uart_num_);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

public:
    DfplayerController(uart_port_t uart_num = UART_NUM_2,
                       gpio_num_t tx_pin = GPIO_NUM_17,
                       gpio_num_t rx_pin = GPIO_NUM_18)
        : uart_num_(uart_num) {
        if (tx_pin == GPIO_NUM_NC || rx_pin == GPIO_NUM_NC) {
            ESP_LOGW(DFPLAYER_TAG, "DFPlayer pins not configured, skipping");
            return;
        }

        uart_config_t uart_config = {
            .baud_rate = 9600,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_param_config(uart_num_, &uart_config));
        ESP_ERROR_CHECK(uart_set_pin(uart_num_, tx_pin, rx_pin,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(uart_num_, 1024, 0, 0, NULL, 0));

        // Select TF card as playback device
        SendCommand(0x09, 0x0002);
        // Set default volume
        SendCommand(0x06, volume_);

        auto& mcp_server = McpServer::GetInstance();

        mcp_server.AddTool("self.dfplayer.play_track",
            "Play a specific track from the DFPlayer's TF card.\n"
            "The track number corresponds to the file order on the TF card.",
            PropertyList({
                Property("track", kPropertyTypeInteger, 1, 1, 2999)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto track = properties["track"].value<int>();
                ESP_LOGI(DFPLAYER_TAG, "Play track: %d", track);
                SendCommand(0x03, track);
                return "{\"track\":" + std::to_string(track) + "}";
            });

        mcp_server.AddTool("self.dfplayer.set_volume",
            "Set the volume of the DFPlayer module (0-30).",
            PropertyList({
                Property("volume", kPropertyTypeInteger, 0, 0, 30)
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                volume_ = properties["volume"].value<int>();
                ESP_LOGI(DFPLAYER_TAG, "Set volume: %d", volume_);
                SendCommand(0x06, volume_);
                return "{\"volume\":" + std::to_string(volume_) + "}";
            });

        mcp_server.AddTool("self.dfplayer.get_volume",
            "Get the current volume of the DFPlayer module.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                return "{\"volume\":" + std::to_string(volume_) + "}";
            });

        mcp_server.AddTool("self.dfplayer.play",
            "Resume playback on the DFPlayer module.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(DFPLAYER_TAG, "Resume playback");
                SendCommand(0x0D);
                return true;
            });

        mcp_server.AddTool("self.dfplayer.pause",
            "Pause playback on the DFPlayer module.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(DFPLAYER_TAG, "Pause playback");
                SendCommand(0x0E);
                return true;
            });

        mcp_server.AddTool("self.dfplayer.stop",
            "Stop playback on the DFPlayer module.",
            PropertyList(),
            [this](const PropertyList& properties) -> ReturnValue {
                ESP_LOGI(DFPLAYER_TAG, "Stop playback");
                SendCommand(0x16);
                return true;
            });
    }

    ~DfplayerController() {
        if (uart_num_ != UART_NUM_MAX) {
            uart_driver_delete(uart_num_);
        }
    }
};

#endif // __DFPLAYER_CONTROLLER_H__
