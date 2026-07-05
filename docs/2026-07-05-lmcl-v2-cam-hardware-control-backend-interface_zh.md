# LMCL Box v2 CAM 硬件控制与后端接口文档

更新时间：2026-07-05  
适用固件项目：`Xiaole_Github_CAM`  
适用硬件板级配置：`lmcl-box-v2-cam`  
主要代码位置：

- `main/mcp_server.cc`：xiaozhi 通用 MCP 工具注册与 JSON-RPC 解析。
- `main/boards/lmcl-box-v2-cam/mcp_controller.cc`：本摄像头硬件新增的 AEC / 重启工具。
- `main/companion_mcp_tools.cc`：Xiaole 自定义设备能力工具，目前主要是闹钟。
- `main/boards/lmcl-box-v2-cam/lmcl_box_v2_cam_board.cc`：摄像头、屏幕、音频、电量、按键等硬件初始化。

## 1. 总体结论

当前固件已经具备以下可以由后端/大模型通过 MCP 调用的能力：

- 读取设备状态：音量、屏幕亮度、主题、电量、充电状态、Wi-Fi 信号、芯片温度。
- 设置扬声器音量。
- 设置屏幕亮度。
- 设置屏幕主题。
- 拍照，并把照片上传到后端下发的视觉解释接口。
- 设置 / 查询 AEC 对话打断模式。
- 重启设备。
- 设置、取消、列出、重命名闹钟。

注意：当前固件不是通过“返回某个关键词字符串”直接触发硬件动作，而是通过 MCP JSON-RPC 的 `tools/call` 调用工具。后端可以在大模型侧把“拍照”“音量调大”“屏幕亮一点”“电量多少”等自然语言意图映射为对应的 MCP 工具调用。

## 2. 硬件配置摘要

| 模块 | 当前配置 |
| --- | --- |
| 主控 | ESP32-S3，板级名 `lmcl-box-v2-cam` |
| 音频输入 | ES7210，I2S/TDM 麦克风输入，采样率 24000 Hz |
| 音频输出 | ES8311，I2S 扬声器输出，采样率 24000 Hz |
| I2S 引脚 | MCLK GPIO38，WS GPIO13，BCLK GPIO14，DIN GPIO12，DOUT GPIO45 |
| I2C | SDA GPIO1，SCL GPIO2，I2C port 1 |
| IO 扩展 | PCA9557 `0x19`，用于屏幕/功放/摄像头电源相关控制 |
| 摄像头 | ESP32 parallel camera，RGB565，VGA，XCLK 24 MHz，PSRAM frame buffer |
| 摄像头引脚 | D0 GPIO7，D1 GPIO4，D2 GPIO5，D3 GPIO6，D4 GPIO8，D5 GPIO9，D6 GPIO11，D7 GPIO15，VSYNC GPIO21，HREF GPIO18，PCLK GPIO16，XCLK GPIO17 |
| 屏幕 | ST7789 SPI LCD，横屏 320x240 或竖屏 240x320 |
| 屏幕 SPI | MOSI GPIO40，SCLK GPIO41，DC GPIO39，CS/RESET 未接 |
| 背光 | PWM GPIO42，输出反相 |
| 按键 | BOOT GPIO0，音量加 GPIO3，音量减 GPIO46 |
| LED | GPIO48 单灯 |
| 电源检测 | 充电状态 GPIO47，电量 ADC1 CH9，内部温度传感器 |

实体按键本地行为：

| 操作 | 固件行为 |
| --- | --- |
| BOOT 单击 | 启动阶段进入配网；已运行时切换对话状态 |
| BOOT 双击 | 首次启动时本地拍照一次；之后在 idle 状态切换 AEC 模式并把音量设为 60 |
| BOOT 长按 | 进入 Wi-Fi 配网 |
| 音量加单击 / 长按 | 音量 +10 / 设置为 100 |
| 音量减单击 / 长按 | 音量 -10 / 静音 |

## 3. MCP 通道格式

后端通过 `/agent/v1` WebSocket 向设备发送文本帧：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "tools/list"
  }
}
```

固件也按同样的外层信封回复：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "result": {
      "tools": []
    }
  }
}
```

推荐后端在设备进入 `ready` 后执行：

1. `initialize`：初始化 MCP，并下发视觉解释接口能力。
2. `tools/list`：读取设备当前实际可用工具。
3. 根据用户语音意图发送 `tools/call`。

### 3.1 初始化与视觉能力下发

拍照工具依赖 `initialize.params.capabilities.vision.url`。如果没有下发，`self.camera.take_photo` 会返回错误：`Image explain URL or token is not set`。

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 1,
    "method": "initialize",
    "params": {
      "capabilities": {
        "vision": {
          "url": "https://api.example.com/vision/explain",
          "token": "DEVICE_OR_VISION_TOKEN"
        }
      }
    }
  }
}
```

固件收到后会保存 `vision.url` 和 `vision.token`，后续拍照时由设备直接 `POST` 图片到该 URL。

### 3.2 获取工具列表

普通对话工具：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 2,
    "method": "tools/list"
  }
}
```

包含后台管理工具：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 3,
    "method": "tools/list",
    "params": {
      "withUserTools": true
    }
  }
}
```

`withUserTools` 会列出 `self.reboot`、`self.upgrade_firmware`、`self.screen.snapshot` 等不建议直接暴露给普通 AI 对话的工具。即使不列出，后端如果知道工具名，也能直接 `tools/call`，所以服务端需要自己做权限和确认策略。

## 4. 当前已可调用的普通工具

### 4.1 `self.get_device_status`

用途：读取实时设备状态。

参数：无。

返回文本内容是一个 JSON 字符串，主要字段：

```json
{
  "audio_speaker": {
    "volume": 60
  },
  "screen": {
    "brightness": 80,
    "theme": "dark"
  },
  "battery": {
    "level": 85,
    "charging": false
  },
  "network": {
    "type": "wifi",
    "ssid": "xxx",
    "signal": "strong"
  },
  "chip": {
    "temperature": 52.9
  }
}
```

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 10,
    "method": "tools/call",
    "params": {
      "name": "self.get_device_status",
      "arguments": {}
    }
  }
}
```

后端适配建议：

- 用户问“电量多少”“在充电吗”“温度多少”“现在音量多少”“屏幕亮度多少”时调用。
- 用户说“音量调大一点”“亮一点”这类相对控制时，先调用本工具读取当前值，再计算目标值后调用设置工具。

### 4.2 `self.audio_speaker.set_volume`

用途：设置扬声器输出音量。

参数：

| 字段 | 类型 | 范围 | 说明 |
| --- | --- | --- | --- |
| `volume` | integer | 0-100 | 0 为静音，100 为最大音量 |

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 11,
    "method": "tools/call",
    "params": {
      "name": "self.audio_speaker.set_volume",
      "arguments": {
        "volume": 60
      }
    }
  }
}
```

后端适配建议：

- “静音” -> `volume: 0`
- “音量最大” -> `volume: 100`
- “音量调到 60” -> `volume: 60`
- “声音小一点/大一点” -> 先 `self.get_device_status`，再按 10 或 20 的步进计算目标音量。

### 4.3 `self.screen.set_brightness`

用途：设置屏幕背光亮度。

参数：

| 字段 | 类型 | 范围 | 说明 |
| --- | --- | --- | --- |
| `brightness` | integer | 0-100 | PWM 背光亮度，0 可能接近黑屏 |

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 12,
    "method": "tools/call",
    "params": {
      "name": "self.screen.set_brightness",
      "arguments": {
        "brightness": 80
      }
    }
  }
}
```

后端适配建议：

- “屏幕亮一点/暗一点” -> 先读当前亮度，再按 10 或 20 的步进计算目标值。
- 普通语音场景建议后端把最低亮度限制在 5-10，避免用户误以为设备死机。

### 4.4 `self.screen.set_theme`

用途：切换 LVGL 屏幕主题。

参数：

| 字段 | 类型 | 可选值 | 说明 |
| --- | --- | --- | --- |
| `theme` | string | `light` / `dark` | 固件会查找对应主题，找不到则返回 `false` |

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 13,
    "method": "tools/call",
    "params": {
      "name": "self.screen.set_theme",
      "arguments": {
        "theme": "dark"
      }
    }
  }
}
```

后端适配建议：

- “切换深色模式” -> `theme: "dark"`
- “切换浅色模式” -> `theme: "light"`

### 4.5 `self.camera.take_photo`

用途：拍摄当前画面，在屏幕上预览，并把照片上传到后端视觉解释接口，然后把视觉接口返回内容作为工具结果返回。

参数：

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `question` | string | 是 | 要问图片的问题，例如“请描述你看到的内容” |

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 14,
    "method": "tools/call",
    "params": {
      "name": "self.camera.take_photo",
      "arguments": {
        "question": "请描述你看到的画面，并回答用户的问题。"
      }
    }
  }
}
```

固件行为：

1. 采集摄像头图像，当前配置为 RGB565 / VGA。
2. 把当前帧设置为屏幕预览图。
3. 将图像编码为 JPEG。
4. 通过 HTTP multipart 上传到 `initialize` 下发的 `vision.url`。
5. 把视觉接口 HTTP 200 响应体原样作为 MCP 工具结果文本返回。

视觉接口请求格式：

```http
POST {vision.url}
Device-Id: {mac}
Client-Id: {device_uuid}
Authorization: Bearer {vision.token}   # token 非空时发送
Content-Type: multipart/form-data; boundary=...
Transfer-Encoding: chunked
```

multipart 字段：

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `question` | text | 后端/模型传给拍照工具的问题 |
| `file` | image/jpeg | 文件名固定为 `camera.jpg` |

视觉接口响应要求：

- HTTP 状态码必须是 `200`。
- 响应体可以是纯文本，也可以是 JSON 字符串；固件不会解析，会原样返回给 MCP 调用方。
- 建议后端返回简短、可直接给大模型继续使用的结果，例如：

```json
{
  "answer": "画面中有一个人坐在桌前，桌上有一个黑色设备。",
  "confidence": 0.86
}
```

后端适配建议：

- “拍张照”“看看前面是什么”“识别一下画面”“你看到了什么” -> 调用本工具。
- 如果用户问题里带目标，例如“帮我看看桌上有什么”，`question` 应包含该目标。
- 如果暂时没有视觉模型，后端也需要先实现一个最小 `vision.url`，哪怕只存图并返回“已收到图片”，否则拍照 MCP 会失败。

### 4.6 `self.AEC.set_mode`

用途：设置 AEC 对话打断模式。

参数：

| 字段 | 类型 | 可选值 | 说明 |
| --- | --- | --- | --- |
| `mode` | string | `kAecOff` / `kAecOnDeviceSide` | 当前实现中，除 `kAecOff` 外的值都会按开启设备侧 AEC 处理 |

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 15,
    "method": "tools/call",
    "params": {
      "name": "self.AEC.set_mode",
      "arguments": {
        "mode": "kAecOnDeviceSide"
      }
    }
  }
}
```

后端适配建议：

- “打开打断”“我想随时打断你” -> `kAecOnDeviceSide`
- “关闭打断”“不要被我说话打断” -> `kAecOff`

### 4.7 `self.AEC.get_mode`

用途：查询当前 AEC 对话打断模式。

参数：无。

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 16,
    "method": "tools/call",
    "params": {
      "name": "self.AEC.get_mode",
      "arguments": {}
    }
  }
}
```

### 4.8 `self.res.esp_restart`

用途：重启 ESP32 设备。

参数：无。

示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 17,
    "method": "tools/call",
    "params": {
      "name": "self.res.esp_restart",
      "arguments": {}
    }
  }
}
```

后端适配建议：

- 该工具会直接重启设备，建议后端要求用户二次确认。
- 普通闲聊模型不要自动调用。

## 5. Xiaole 自定义闹钟工具

这些工具已经在 `main/companion_mcp_tools.cc` 注册，可通过同一个 MCP 通道调用。

| 工具名 | 作用 |
| --- | --- |
| `alarm.set` | 设置倒计时或指定时间闹钟 |
| `alarm.cancel` | 按 `id` 或 `name` 取消闹钟 |
| `alarm.cancel_all` | 取消全部闹钟 |
| `alarm.list` | 列出待触发闹钟 |
| `alarm.rename` | 重命名闹钟 |

### 5.1 `alarm.set`

参数：

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| `type` | string | `relative` | `relative` 或 `time_of_day`，也兼容 `countdown`、`timer`、`time` |
| `delay_seconds` | integer | 60 | 倒计时秒数 |
| `hour` | integer | 0 | 指定时间小时，0-23 |
| `minute` | integer | 0 | 指定时间分钟，0-59 |
| `name` | string | `闹钟` | 闹钟名称 |
| `ring_duration` | integer | 固件配置 | 响铃秒数，1-300 |

倒计时示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 20,
    "method": "tools/call",
    "params": {
      "name": "alarm.set",
      "arguments": {
        "type": "relative",
        "delay_seconds": 300,
        "name": "五分钟提醒",
        "ring_duration": 30
      }
    }
  }
}
```

指定时间示例：

```json
{
  "type": "mcp",
  "payload": {
    "jsonrpc": "2.0",
    "id": 21,
    "method": "tools/call",
    "params": {
      "name": "alarm.set",
      "arguments": {
        "type": "time_of_day",
        "hour": 7,
        "minute": 30,
        "name": "起床"
      }
    }
  }
}
```

## 6. 后台/用户管理工具

这些工具由 `AddUserOnlyTools()` 注册，默认 `tools/list` 不返回；需要 `withUserTools: true` 才会列出。建议只在 App 管理页、后台控制台或用户明确授权后使用。

| 工具名 | 参数 | 作用 |
| --- | --- | --- |
| `self.get_system_info` | 无 | 获取系统信息 |
| `self.reboot` | 无 | 应用层重启设备 |
| `self.upgrade_firmware` | `url` | 从指定 URL 下载固件并 OTA 升级 |
| `self.screen.get_info` | 无 | 获取屏幕宽高、是否黑白屏 |
| `self.screen.snapshot` | `url`, `quality` | 截屏为 JPEG 并上传到指定 URL；依赖 `CONFIG_LV_USE_SNAPSHOT` |
| `self.screen.preview_image` | `url` | 下载图片并在屏幕预览；依赖 `CONFIG_LV_USE_SNAPSHOT` |
| `self.assets.set_download_url` | `url` | 设置资源下载 URL |

## 7. 语音意图到 MCP 工具映射建议

| 用户说法 / 关键词 | 后端应识别的意图 | MCP 工具 | 参数策略 |
| --- | --- | --- | --- |
| 拍张照、看一下、你看到了什么、识别画面 | 视觉拍照问答 | `self.camera.take_photo` | `question` 填用户原始问题或补成“请描述画面” |
| 音量多少、现在声音多大 | 查询设备状态 | `self.get_device_status` | 无 |
| 音量调到 60、声音设成一半 | 设置绝对音量 | `self.audio_speaker.set_volume` | `volume` 0-100 |
| 声音大一点/小一点 | 设置相对音量 | 先 `self.get_device_status`，再 `self.audio_speaker.set_volume` | 建议步进 10 |
| 静音 | 设置音量为 0 | `self.audio_speaker.set_volume` | `volume: 0` |
| 屏幕亮一点/暗一点 | 设置相对亮度 | 先 `self.get_device_status`，再 `self.screen.set_brightness` | 建议步进 10 |
| 亮度调到 80 | 设置绝对亮度 | `self.screen.set_brightness` | `brightness: 80` |
| 深色模式/浅色模式 | 设置主题 | `self.screen.set_theme` | `dark` / `light` |
| 电量多少、在充电吗 | 查询电量 | `self.get_device_status` | 读 `battery.level` / `battery.charging` |
| 芯片温度多少、设备热不热 | 查询温度 | `self.get_device_status` | 读 `chip.temperature` |
| 打开打断、关闭打断 | 设置 AEC | `self.AEC.set_mode` | `kAecOnDeviceSide` / `kAecOff` |
| 现在能不能打断 | 查询 AEC | `self.AEC.get_mode` | 无 |
| 重启设备 | 重启 | `self.res.esp_restart` 或 `self.reboot` | 建议二次确认 |
| 五分钟后提醒我 | 设置闹钟 | `alarm.set` | `type: relative`, `delay_seconds: 300` |
| 明早七点半叫我 | 设置闹钟 | `alarm.set` | `type: time_of_day`, `hour: 7`, `minute: 30` |
| 取消闹钟、取消所有提醒 | 取消闹钟 | `alarm.cancel` / `alarm.cancel_all` | 有具体名称/id 时用 `alarm.cancel` |

## 8. 后端需要配合实现的内容

### 8.1 WebSocket MCP 帧处理

后端 `/agent/v1` 需要支持：

- 接收设备上行的 `{"type":"mcp","payload":...}` 响应。
- 向设备下发 `{"type":"mcp","payload":...}` 请求。
- 维护 JSON-RPC `id` 与调用结果的对应关系。
- 支持 `initialize`、`tools/list`、`tools/call` 调用链。

当前固件已经能解析下行 `type=mcp`，但此前项目契约中后端还没有启用该通道，因此后端需要补齐。

### 8.2 大模型工具调用

后端推荐流程：

1. 设备 WebSocket ready 后，向设备发送 MCP `initialize`。
2. 向设备发送 MCP `tools/list`，把返回的工具 schema 注册到 LLM function calling / tool calling。
3. 当 LLM 选择设备工具时，后端把工具名和参数转为 MCP `tools/call` 发给设备。
4. 收到设备 MCP 结果后，把结果回填给 LLM，让 LLM 组织最终回复。

### 8.3 视觉解释接口

后端必须实现并在 `initialize.capabilities.vision.url` 下发图片解释接口，否则 `self.camera.take_photo` 只能本地拍照预览，无法完成工具调用。

接口要求见 4.5。建议后端至少支持：

- 接收 multipart `question` 和 `file`。
- 校验 `Authorization: Bearer {vision.token}`。
- 把图片送入视觉模型。
- 返回简短文本或 JSON。

### 8.4 权限与确认

后端应做安全策略：

- `self.res.esp_restart`、`self.reboot`、`self.upgrade_firmware` 需要用户二次确认。
- `self.screen.set_brightness` 不建议允许 0 作为普通语音自动结果。
- `self.camera.take_photo` 涉及摄像头，建议在 App/用户协议中明确授权。
- `withUserTools: true` 的工具不要直接暴露给普通模型自由调用。

## 9. 硬件具备但当前还需要固件和后端一起定义的功能

以下能力硬件或上游代码具备基础条件，但当前 `lmcl-box-v2-cam` 尚未作为普通 MCP 工具完整暴露，建议后续按需增加。

| 功能 | 当前状态 | 需要定义的接口 |
| --- | --- | --- |
| Wi-Fi 重新配网 | 实体 BOOT 长按已支持；当前 CAM 板没有注册 `self.system.reconfigure_wifi` | 新增 MCP 工具，例如 `self.system.reconfigure_wifi`，后端做确认后调用 |
| 屏幕横竖屏切换 | 固件启动时读取 NVS `lcd_display/lcd_mode`，但没有 MCP 工具 | 定义 `self.display.set_mode`，参数 `landscape` / `portrait`，可能需要重启或重新初始化屏幕 |
| 表情/GIF 模式 | 上游 README 提到 `self.gif.set_gif_mode`，但当前 CAM 板代码没有注册该工具 | 明确表情资源格式、资源 URL、表情枚举、播放/停止策略 |
| LED 控制 | 板上有 GPIO48 单灯，但当前没有 MCP 工具 | 定义 `self.led.set_state`、`self.led.blink`、亮度/颜色能力需按实际 LED 类型确认 |
| 摄像头参数 | 底层 `Esp32Camera` 已有 `SetHMirror`、`SetVFlip`、`SetSwapBytes`，但没有 MCP 工具 | 定义 `self.camera.set_options`，包括镜像、翻转、质量、分辨率等 |
| 拍照只上传/只保存 | 当前 `self.camera.take_photo` 固定走视觉解释接口 | 定义独立 `self.camera.capture`，返回图片 URL、文件 ID 或上传结果 |
| 电量/充电/温度主动事件 | `PowerManager` 有状态回调基础，但未接入后端事件 | 定义低电量、充电状态变化、温度过高的 `deviceEvent` 或 HTTP 上报格式 |
| 麦克风/扬声器高级控制 | 当前只有音量和 AEC 开关 | 定义输入增益、输出开关、麦克风静音、回声消除模式枚举 |
| 屏幕显示图片/文字 | 后台工具有条件支持 `preview_image`，普通对话未暴露 | 定义 `self.screen.show_text`、`self.screen.show_image`、展示时长、下载鉴权 |
| 资源下载与表情包 | 已有 `self.assets.set_download_url` 管理工具 | 后端需要定义资源 manifest、版本、hash、缓存更新策略 |

## 10. 与现有控制面心跳的关系

当前固件已经通过 HTTP 控制面周期上报 `ReportDeviceStatus`，字段包括：

```json
{
  "firmwareVersion": "x.x.x",
  "deviceModelCode": "lmcl-box-v2-cam",
  "powerSource": "POWER_SOURCE_BATTERY",
  "batteryLevel": 85,
  "extra": {
    "boardName": "lmcl-box-v2-cam",
    "mac": "xx:xx:xx:xx:xx:xx"
  }
}
```

这个心跳用于后端在线状态、设备归属和基础电量展示；MCP `self.get_device_status` 用于对话过程中的实时查询和控制前置读取。两者不冲突，建议后端都保留。

## 11. 后端最小实现清单

第一阶段建议至少完成：

- 支持 WebSocket `type=mcp` 下行请求和上行响应。
- 连接 ready 后发送 MCP `initialize`，并下发 `vision.url/token`。
- 支持 `tools/list` 并把工具 schema 接入 LLM。
- 支持 `tools/call` 到设备，并把结果返回给 LLM。
- 实现图片解释 HTTP 接口。
- 配置自然语言到工具调用的意图策略：拍照、音量、亮度、电量、温度、AEC、闹钟、重启确认。

第二阶段再补：

- Wi-Fi 重新配网 MCP。
- 表情/GIF MCP。
- LED MCP。
- 摄像头参数 MCP。
- 设备主动事件上报。
- 屏幕图片/文字展示工具。
