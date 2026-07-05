# Plan 6 契约匹配状态说明

> 本文用于对照 `docs/2026-07-01-companion-plan-6-esp32-firmware.md`，记录当前 CAM 合并项目对 Plan 6 固件接入契约的实现状态。
>
> 当前默认板型为 `lmcl-box-v2-cam`。`device_identity` 的开发期自动 fallback 暂时保留，便于调试阶段不依赖完整产线烧录流程。

---

## 0. 本次范围

**本次已按契约补齐：**
- 控制面 HTTP `401` 后自动重新 Bootstrap，并重试当前 RPC 一次。
- 未绑定配对轮询期间继续按 `heartbeatSeconds` 发送 `ReportDeviceStatus` 心跳。
- `/agent/v1` 文本下行帧增加 `turnId` 过滤，丢弃非当前轮的迟到帧。
- 应用层处理 `intent{kind:"SLEEP"}`，收到后停止当前播报并回到待唤醒。
- 移除旧 `tts` 入站帧分支，WSS 对话帧更贴近新契约帧集。

**本次明确暂不修改：**
- `device_identity` 在 NVS 缺少身份时仍可生成调试身份：`orgId` 使用 `CONFIG_DEVICE_FACTORY_ORG_ID`，`sn` 使用 `CONFIG_LMCL_SN_PREFIX + MAC`，`secret` 使用随机值。量产前需要改为只读出厂预置身份。

---

## 1. 已匹配的契约点

### A. HTTP 控制面

- 已实现 `main/protocols/companion_http_control.{h,cc}`。
- `GetPairingState`、`ReportDeviceStatus`、`CheckFirmwareUpdate` 均通过 HTTP POST 调用。
- 请求头已带 `Authorization: Bearer <deviceToken>`。
- 控制面 base URL 由 Bootstrap URL 推导。
- `heartbeatSeconds` 由 Bootstrap 下发，默认 60 秒。
- HTTP `401` 会触发重新 Bootstrap，刷新令牌后重试当前 RPC 一次。

### B. Bootstrap 与配对流程

- Bootstrap 请求包含 `orgId`、`sn`、`secret`。
- Bootstrap 响应读取 `deviceToken`、`config.endpoints.websocket`、`config.heartbeatSeconds`。
- 未绑定时不直接连接 `/agent/v1`，而是通过 `GetPairingState` 短轮询显示配对码。
- `phase=PAIRING_PHASE_BOUND` 后停止配对等待并连接 `/agent/v1?token=<deviceToken>`。
- 配对等待期间现在会继续发送心跳，避免长时间待绑定时后端在线状态变旧。

### C. Agent WSS 帧集

- `/agent/v1` 连接后等待服务端 `ready`。
- `ready.data.audio.sampleRate` 会更新下行音频采样率。
- `ready.data.voiceEnabled=false` 时，设备不发送 `listen`，并丢弃上行音频。
- 上行音频帧格式为 `[0x01][Opus bytes...]`。
- 下行音频帧格式为 `[0x01][turnId low8][Opus bytes...]`。
- 下行文本帧根据 `data.turnId` 维护当前轮次；非当前轮的迟到文本帧会被丢弃。
- `listen` 上行仅发送 `{type:"listen",data:{state:"start"}}`。
- `abort` 上行已禁用，手动停止只做端侧本地停播。
- `intent{kind:"SLEEP"}` 已处理，设备回到待唤醒。

### D. OTA

- 固件更新检测由 HTTP `CheckFirmwareUpdate` 触发。
- OTA 下载、`packageSize` 校验、SHA256 校验、刷写和 rollback valid 标记继续复用 `CompanionOta`。
- 当前仍未实现固件签名校验与 anti-rollback 硬化，保持 Plan 6 中“后续上市硬门槛”的定位。

### E. 音频与构建

- Opus 帧长保持 20 ms。
- 上行编码保持 16 kHz。
- 下行解码按服务端 `ready.audio.sampleRate`，默认 24 kHz。
- `CONFIG_USE_DEVICE_AEC=y`。
- 默认构建板型为 `CONFIG_BOARD_TYPE_LMCL_BOX_V2_CAM=y`。
- `CONFIG_DEVICE_MODEL_CODE="lmcl-box-v2-cam"`。
- 分区表为 `partitions/v2/16m.csv`。

---

## 2. 调试期保留差异

### A. 出厂身份 fallback

Plan 6 严格契约要求：设备没有出厂烧录的 `identity/orgId`、`identity/sn`、`identity/secret` 时，不应自动生成身份，应进入“未出厂配置”错误态。

当前调试阶段保留：
- `orgId` 缺失时使用 `CONFIG_DEVICE_FACTORY_ORG_ID`。
- `sn` 缺失时使用 `CONFIG_LMCL_SN_PREFIX + MAC`。
- `secret` 缺失时生成随机 32 字节 hex。
- 这些 fallback 会写入 NVS，便于开发板和样机联调。

量产前需要关闭该 fallback，并配套产线工装：
- 生成 `(orgId, sn, secret)`。
- 烧录到 NVS、efuse 或只读分区。
- 后端预登记或 TOFU 建档策略最终确认。

### B. 硬件安全

当前 OTA 只做 URL 下载、大小校验和 SHA256 校验。尚未实现：
- 固件签名校验。
- efuse 固化公钥。
- anti-rollback。
- secret 的 efuse/只读分区硬化。

这些属于上市前安全硬化项。

---

## 3. 后续建议

1. 调试完成后，移除 `device_identity` fallback，改为严格只读出厂身份。
2. 和后端确认 `intent.kind` 的最终枚举字符串是否固定为 `"SLEEP"`。
3. 做硬件冒烟测试：Bootstrap、配对轮询、绑定后 WSS、语音一轮、打断、`intent{SLEEP}`、心跳、OTA 检查。
4. 做 401 测试：让后端令牌过期，确认控制面 RPC 会自动 Bootstrap 并重试成功。
5. 上市前补 OTA 签名、anti-rollback 和身份密钥硬化。
