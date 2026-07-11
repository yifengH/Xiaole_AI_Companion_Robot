# 小智 ESP32 微信聊天气泡界面移植指南

本文说明如何把本项目的微信聊天界面移植到另一块使用 LVGL 的小智 ESP32 板型。示例基于官方 `78/xiaozhi-esp32` 的 `LcdDisplay`，适用于彩色 LCD；OLED 或自定义 `Display` 子类需要单独适配。

## 1. 实现效果与消息链路

- 用户语音识别文本：右侧微信绿色气泡。
- 助手回复文本：左侧白色气泡。
- 服务端分段发送的回复：每个文本段生成一个独立气泡。
- 长文本：在气泡最大宽度处自动换行。
- 新消息：自动滚动到可视区域底部。
- 历史消息：ESP32-S3 默认最多保留 20 条，防止 LVGL 对象持续占用内存。
- 微信模式关闭自定义表情和居中的 AI 图标，避免覆盖聊天气泡；其他显示模式不受影响。

协议消息在 `main/application.cc` 中进入显示层：

```cpp
display->SetChatMessage("assistant", message.c_str());
display->SetChatMessage("user", message.c_str());
```

因此移植通常不需要修改 WebSocket、ASR 或 TTS，只需启用并调整 `LcdDisplay`。

## 2. 确认目标板型使用 LcdDisplay

找到目标板初始化显示屏的位置，例如：

```cpp
display_ = new SpiLcdDisplay(panel_io, panel, width, height,
                             offset_x, offset_y,
                             mirror_x, mirror_y, swap_xy);
```

`SpiLcdDisplay`、`RgbLcdDisplay` 和 `MipiLcdDisplay` 都继承自 `LcdDisplay`，可以直接使用本界面。如果板型实例化的是 `OledDisplay`、`EmoteDisplay` 或完全自定义的显示类，不能只打开配置开关。

同时确认：

- 工程启用了 LVGL。
- 中文字体包含需要显示的字符。
- 屏幕逻辑分辨率和旋转后的宽高正确。
- `SetupUI()` 最终调用的是 `LcdDisplay::SetupUI()`。

## 3. 为板型启用微信样式

推荐修改目标板的 `main/boards/<board-name>/config.json`，这样使用发布脚本重新构建时配置不会丢失：

```json
{
  "target": "esp32s3",
  "builds": [
    {
      "name": "your-board",
      "sdkconfig_append": [
        "CONFIG_USE_WECHAT_MESSAGE_STYLE=y"
      ]
    }
  ]
}
```

手动构建的工作区也可以通过 `idf.py menuconfig` 选择：

```text
Xiaozhi Assistant
  -> Display Style
  -> Enable WeChat Message Style
```

最终 `sdkconfig` 应包含：

```text
# CONFIG_USE_DEFAULT_MESSAGE_STYLE is not set
CONFIG_USE_WECHAT_MESSAGE_STYLE=y
```

不要同时启用默认消息样式和微信消息样式；它们属于同一个 Kconfig `choice`。

## 4. 核心界面代码

实现位于 `main/display/lcd_display.cc` 的以下条件编译块：

```cpp
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() { /* 状态栏和滚动聊天容器 */ }
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    /* 创建、着色、对齐气泡并滚动 */
}
void LcdDisplay::ClearChatMessages() { /* 清除历史 */ }
#else
/* 默认字幕界面 */
#endif
```

本项目采用的亮色主题参数为：

```cpp
chat_background_color = 0xEDEDED; // 微信灰背景
user_bubble_color      = 0x95EC69; // 用户绿色气泡
assistant_bubble_color = 0xFFFFFF; // 助手白色气泡
```

气泡建议使用不透明背景：

```cpp
lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_COVER, 0);
```

最大文本宽度可按横向分辨率缩放：

```cpp
lv_coord_t max_width = LV_HOR_RES * 76 / 100;
lv_obj_set_width(msg_text, max_width);
lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
```

角色到布局的映射：

| role | 位置 | 背景 |
|---|---|---|
| `user` | 右侧 | `user_bubble_color` |
| `assistant` | 左侧 | `assistant_bubble_color` |
| `system` | 居中 | `system_bubble_color` |

## 5. 不同分辨率的调参建议

不要复制固定的 320×240 坐标。优先使用 `LV_HOR_RES`、`LV_VER_RES`、`LV_PCT(100)` 和主题间距。

| 逻辑分辨率 | 最大气泡宽度 | 页面边距 | 字号建议 |
|---|---:|---:|---:|
| 240×240 | 72%～76% | 6～8 px | 14～18 px |
| 320×240 | 74%～78% | 8～10 px | 16～20 px |
| 320×480 | 76%～82% | 10～14 px | 18～24 px |
| 480×480 及以上 | 70%～78% | 14～20 px | 22～30 px |

如果横竖屏方向不符，先修正板级 `swap_xy`、`mirror_x`、`mirror_y`，不要用气泡坐标补偿屏幕旋转错误。

## 6. 回复分段策略

本项目把每次收到的 `assistant` 文本作为一个自然段气泡。小智服务端通常按 TTS 文本段发送，因此能自然形成多个白色气泡，并且不会在设备端破坏 UTF-8 中文。

若另一个服务端一次返回整篇长文，优先让服务端按语义段落发送多个 TTS 文本事件。设备端按标点强制拆分需要额外处理 UTF-8、多字节标点、数字小数点、英文缩写和代码文本，容易产生错误分段，不建议作为默认方案。

如果必须在设备端拆分，应先按原有换行分段，再在超过设定字符数时寻找最近的 `。！？；\n`，并限制单次创建的气泡数量。

## 7. 内存与稳定性

- ESP32-S3 建议保留不超过 20 条消息。
- 删除旧消息要删除最外层消息容器，避免子对象泄漏。
- 所有 LVGL 创建、删除、改样式操作必须处于显示锁中。
- `content == nullptr` 和空字符串必须提前处理。
- `lv_obj_set_user_data()` 中保存的角色标记应使用静态字符串，不要保存临时 `std::string::c_str()`。
- 图片消息需在 LVGL 对象删除事件中释放底层图片对象。
- 不要在每个音频帧或每个流式 token 上创建气泡，只在完整文本段到达时创建。

### 关闭遮挡聊天区的表情

部分板型的 `emoji_image_` 和 `emoji_label_` 直接挂在活动屏幕上，层级高于聊天容器，因此较大的 PNG/GIF 会覆盖气泡。微信模式应在 `SetupUI()` 创建对象后立即隐藏：

```cpp
lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
```

还必须在 `SetEmotion()` 的微信模式分支中持续隐藏并直接返回，否则后续服务端 emotion 消息会再次显示图片：

```cpp
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
DisplayLockGuard lock(this);
if (gif_controller_) {
    gif_controller_->Stop();
    gif_controller_.reset();
}
if (emoji_image_ != nullptr) {
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
}
if (emoji_label_ != nullptr) {
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
}
return;
#endif
```

保留对象而不是删除对象，是因为状态更新、主题切换等通用代码仍可能引用它们。这样可以关闭视觉显示，同时保持接口兼容和空指针安全。

## 8. 构建和验证

推荐通过目标板发布配置构建：

```powershell
python ./scripts/release.py <board-name>
```

或者在已经选择目标板的工作区构建：

```powershell
idf.py build
```

烧录后至少验证：

1. 用户短句显示在右侧绿色气泡。
2. 助手短句显示在左侧白色气泡。
3. 中文长句能换行且不越过屏幕边缘。
4. 连续对话会自动滚动，新消息完整可见。
5. 超过历史上限后没有重启或明显内存持续下降。
6. 切换亮/暗主题后文字仍有足够对比度。
7. 空的系统状态更新不会生成空白气泡。
8. 拍照预览、低电量弹窗和状态栏仍能正常显示。
9. 服务端连续发送不同 emotion 后，自定义表情和 AI 图标仍不会覆盖聊天区域。

## 9. 常见问题

### 打开开关后仍是默认字幕

检查实际参与构建的 `sdkconfig`，以及板型发布脚本是否重新生成了独立构建目录。旧 `build` 目录可能仍对应之前的配置。

### 气泡左右位置不正确

消息外层容器应为 `LV_PCT(100)` 宽，用户气泡在容器内 `LV_ALIGN_RIGHT_MID`；不要直接依赖聊天容器的交叉轴对齐。

### 白色气泡看起来是灰色

检查是否仍使用 `LV_OPA_70`。微信风格应使用 `LV_OPA_COVER`，否则白色会和灰色页面背景混合。

### 中文显示方框

这属于字体字库问题，与气泡布局无关。重新生成包含所需中文字符的 LVGL 字体资源，并确认主题使用了该字体。

### 消息多了以后重启

降低 `MAX_MESSAGES`，检查 PSRAM 和堆内存，并确认删除的是整个消息容器。还要避免把流式 token 当成独立消息。
