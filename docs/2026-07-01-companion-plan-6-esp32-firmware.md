# Plan 6：ESP32 固件重构（对齐新后端契约）

> **注（2026-07-02）**：backend `docs/device-access.md` 已删除——设备/实时契约的真相源现为 `proto/agent_realtime/agent_realtime_core_model.proto` + `proto/companion_device/companion_device.proto` 的 proto 注释（开放平台接入文档归开放平台自身，不入库）。本文档后续出现的 `device-access.md` 引用，一律按「proto 契约」理解。

> 全栈重构第 6 份、收尾一份。把陪伴设备「小乐」ESP32 固件从旧模型（一条 `/device/v1` WSS 承载配对/状态/OTA/对话全部）改成新契约：**(A) HTTP 控制面**（Bootstrap + 配对短轮询 + 心跳 + OTA，全 HTTP RPC，带设备令牌）+ **(B) 仅绑定后连 `/agent/v1` WSS**（纯对话，帧集对齐）。
>
> **落点仓：`D:\workspace\Xiaole_AI_Companion_Robot`**（fork of `78/xiaozhi-esp32`，ESP-IDF，C/C++）。**本计划文档暂存于 backend 便于统筹；实施时复制到固件仓 `docs/`。**
> 契约源：backend `docs/superpowers/specs/2026-06-30-companion-platform-architecture-design.md`（§3 帧协议、§4 控制面、§6.2 S1 预登记、§7 端集成矩阵 ESP32 行、§10 D7/D8/D9/D11）+ 冻结的 `docs/device-access.md`（Plans 1-5 落地后）。
> 现状依据：2026-07-01 侦察固件仓（`companion_protocol.cc`、`application.cc`、`companion_ota.cc`、`device_identity.cc`、`Kconfig.lmcl`、`docs/lmcl-upstream-sync-and-isolation.md` —— file:line 已核对）。
> **强前置：backend 契约冻结**——Plans 1-5 落地并验证、`device-access.md` 定稿后才动固件（否则边改边变契约）。**注意 `/device/v1` 已在 Plan 3（clean-cut）删除**——本 plan 之前，测试样机走旧路径已不可用（0 上市设备可控：靠保留的旧固件构建或 backend test-client 验证）；**固件改到新契约（HTTP 控制面 + `/agent/v1`）是恢复设备可用的唯一路径**（见 §6 时序）。

---

## 0. 本 Plan 的范围

**做（本 plan，全部落 lmcl 隔离层——新文件为主，共享 upstream 核心走已登记 Tier-3 hook / keep-ours）：**
- **控制面从 WSS 帧改 HTTP RPC**：新建 `protocols/companion_http_control.{h,cc}`——`ReportDeviceStatus`（心跳，全生命周期定时）、`GetPairingState`（未绑定期短轮询拿配对码/绑定状态）、`CheckFirmwareUpdate`（OTA 拉模型），均 `POST` + `Authorization: Bearer <deviceToken>`。
- **Bootstrap 改出厂预登记**（S1）：`device_identity.{h,cc}` 从「首次开机 `esp_random` 自生成 sn+secret」改「**读出厂预置 sn+secret**」（NVS/efuse 工厂烧录），配套**制造流程**：出厂即把 `(sn, factory_secret_hash, org_id)` 预登记到 backend（Plan 5 `open_api/device.PreregisterDevice` 或控制台）。删设备自注册 TOFU 依赖。
- **Agent 面 WSS 帧集对齐**：`companion_protocol.{h,cc}` 收敛为「仅 Bootstrap + `/agent/v1` 对话」——server `hello`→`ready`；`sleep`→`intent{SLEEP}`；**删 `abort` 上行帧**（手停纯本地停播，D7）；`listen` 仅 `start`（D8）；**全下行帧读 `turnId`**（据此丢弃已作废轮迟到帧）；`pairingCode`/`bound`/`update`/`status`/`checkUpdate` 帧从 WS 移除（走控制面 HTTP）。
- **配对 UX 改轮询驱动**：配对码来自 `GetPairingState` HTTP 响应（非 WSS `pairingCode` 帧）；绑定检测 = 轮询见 `phase=BOUND`（非 `bound` 帧）→ 停配对显示 → 携令牌自连 `/agent/v1`（取代 `NotifyDeviceBound→bound` 反向回调）。
- **设备令牌生命周期**：Bootstrap 换 1h 令牌，存 NVS/RAM；控制面/Agent 面收 `401` → 重 Bootstrap 续。
- **application.cc 帧分发 hook 调整**（Tier-3 keep-ours）：删 `pairingCode`/`status`/`checkUpdate`/`update`/`abort`/`sleep`→旧语义，加 `ready`/`turnEnd`/`intent{SLEEP}` 处理 + turnId 感知；接 HTTP 心跳/配对轮询/OTA 定时器。
- **文档**：固件仓 `docs/` 更新连接契约说明 + 同步 backend `device-access.md` 的设备侧实现。

**不做（不动 / 另案）：**
- **音频管线不动**：AEC（`kAecOnDeviceSide`，`no_audio_codec.cc` 参考通道 + ESP-AFE）/ Opus / 上行 16k·下行 24k·20ms **已匹配新契约**（§3.1 冻结点），零改动。
- **不手改 upstream 共享核心**（`websocket_protocol.*`/`mqtt_protocol.*`/`protocol.h` 基类/reference boards）——新能力=新文件；`application.cc`/`audio_service` 的改动走**已登记 Tier-3 hook**（`docs/lmcl-upstream-sync-and-isolation.md` §8.2），merge upstream 时 keep-ours。
- **视觉帧 `0x02`**：`lmcl-box-v1` 无摄像头 → 本期不实现上行 JPEG（协议留着，将来带摄像头板子再接）。
- **端侧按钮长按 push-to-talk / 隐私关麦**：YAGNI（D8），后端 `listen.state` 已留缝。
- **量产级密钥安全**（efuse 固化厂商公钥验签 OTA、anti-rollback）：backend 上市硬门槛（§4.5），固件侧配合项列 §7 checklist，本期先做 HTTP 链路 + SHA256（`companion_ota.cc` 已有）。

---

## 1. 关键设计判断（已定默认，可推翻）

1. **控制面独立于长连接**：`companion_http_control` 是纯 HTTP 客户端（复用固件已有 HTTP 栈），不依赖任何 WS。心跳/配对轮询/OTA 各自定时器，与 `/agent/v1` 是否在连**正交**（配对期、通话期心跳照发，§4.3 F26）。
2. **`/agent/v1` 仅绑定后连**：未绑定设备**不连 WS**（旧模型的 `pending` WSS 态被删）——未绑定期只跑 HTTP 控制面（Bootstrap→轮询配对→显示码）。见 `phase=BOUND` 才升级 WS。
3. **Bootstrap 出厂预登记是硬前置**（S1）：设备**不再自生成身份**。sn+secret 工厂烧录、backend 预登记。**这改变制造流程**——需一个出厂工装/脚本：生成 `(sn, secret)`→烧 NVS→调 backend 预登记 API 写 `(sn, secret_hash, org_id)`。**待确认**：secret 存 NVS（可擦，量产临时方案）还是 efuse（不可逆，量产正式）；默认 NVS + §7 标注 efuse 为量产硬化。
4. **帧对齐一次到位**：因 backend 契约冻结后固件才动，帧集按最终契约实现（不留旧帧兼容分支）——`abort` 不发、`listen` 仅 start、全下行读 turnId、控制面帧移除。
5. **改动最小化 upstream 冲突面**：所有新逻辑进 `companion_http_control.{h,cc}`（全新文件、零冲突）+ 现有 lmcl 文件（`companion_protocol`/`device_identity`）；`application.cc` 只加**已登记 hook**（帧分发 switch + 定时器起停），不重构其状态机。
6. **令牌 `401` = 重 Bootstrap 信号**：backend `DeviceAuth` 对过期令牌直接回 HTTP `401`（§4.1）；固件所有控制面调用统一拦 `401`→清令牌→重 Bootstrap→重试（有限次退避，进锁定态不无限重试，S1）。

---

## 2. 当前状态速览（已核对，file:line 为真实位置）

**隔离层纪律**（`docs/lmcl-upstream-sync-and-isolation.md`）：fork 起于 upstream `cc7cbe7`；`git merge upstream/main`（非 rebase）；**Tier-1 全隔离新文件**（零冲突）：`protocols/companion_protocol.{h,cc}`、`companion_ota.{h,cc}`、`companion_mcp_tools.{h,cc}`、`device_identity.{h,cc}`、`boards/lmcl-box-v1/*`、`Kconfig.lmcl`；**Tier-3 已登记 hook**（共享文件、keep-ours）：`application.cc` 帧分发、`audio_service` Opus 调参、codecs AEC（§8.2，17 处）。

**Bootstrap + WS（`main/protocols/companion_protocol.cc`）**：`OpenAudioChannel()`(:122-248)：Bootstrap（:274-336，`POST {sn,secret}` 到 `CONFIG_DEVICE_BOOTSTRAP_URL`（`Kconfig.lmcl:5-12` 默认 `https://api.lmcl.xyz/grpc-gateway/CompanionDeviceService/Bootstrap`）→ 读 `{code,message,data:{deviceToken,config:{endpoints:{websocket}}}}`）→ `CreateWebSocket`→`Connect(url + "?token=<deviceToken>")`(:332)→ **连后即发 `status`(:338-361) + `checkUpdate`(:363-371) WSS 帧** → 等 server `hello`(:250-271)。sn/secret 取 NVS `websocket.sn`/`.secret` 或 `DeviceIdentity`(:277-278)。文本帧 `{type,data}`(:152-160,346-410)；二进制 `0x01`=Opus(:80-85,143-151)；server sample rate 默认 24k、从 hello `audio.sampleRate` 取(:19,264)。保活：无设备侧 ping、靠 server ~60s ping + 库 auto-pong；应用层超时 300s(:106-114，**⚠库是否随 server ping 刷新未验证**)。

**帧分发（`main/application.cc`）**：switch server 帧：`tts`(:562-578)/`audioStart|turnStart`(:579)/`audioEnd|turnEnd`(:581)/`subtitle`(:583)/`stt`(:589-608，可打断进行中 TTS)/`sleep`(:615-628，回 idle)/`update`(:629-647，→`UpgradeFirmware`)/`error`(:648)/`mcp`(:609)/`deviceEvent`(:399-410 上行)。上行：`listen{state:start}`(:373-381)、`abort`(:384-393，**仅手停 kAbortReasonNone 发、唤醒词/TTS 打断不发**)、`status`、`checkUpdate`。

**设备身份（`main/device_identity.cc`）**：**首次开机 `esp_random` 自生成**——sn=`CONFIG_LMCL_SN_PREFIX`(默认 `lmcl-`)+24hex(:56-71)、secret=64hex(32B)；存 NVS 命名空间 `identity` 键 `sn`/`secret`，非 MAC 派生，NVS 擦除才重生成(:19-21 量产警告：需 efuse/只读分区预置)。

**OTA（`main/companion_ota.cc`）**：`UpgradeFirmware(url,hash,size,version)`——流式下载边算 SHA256(:61-231)比对 `packageHash`（大小写不敏感）、比 Content-Length vs `packageSize`、`esp_ota_*` 刷写、`MarkCurrentVersionValid`(:41-59) 提交防回滚。**无签名/anti-rollback**。

**音频（`main/audio/audio_service.{h,cc}`）**：编码 16k Opus 20ms(:90,h:39)、解码 24k 20ms(:76,82)；二进制 `[0x01|Opus]`(:76-86)；AEC 编译期 `CONFIG_USE_DEVICE_AEC` XOR `CONFIG_USE_SERVER_AEC`(:58-66)。**契约点全已匹配**。

**构建**：默认板 `CONFIG_BOARD_TYPE_LMCL_BOX_V1`(`sdkconfig.defaults:39`)；型号码 `lmcl-box-v1`(`Kconfig.lmcl:24`，status 上报)；分区 `partitions/v2/16m.csv`（支持 OTA+rollback）；版本串 `esp_app_get_description()->version`(:349)。

---

## 3. 工作项

每项给：**改哪** / **怎么改** / **验证**。全部落 lmcl 隔离层；`application.cc`/`audio_service` 改动为已登记 Tier-3 hook（merge upstream keep-ours）。

### A. 出厂预登记身份（S1）

**A1. `device_identity.{h,cc}` —— 自生成改读出厂预置**
- 删「首次开机 `esp_random` 自生成」（:56-71）；改 `GetSerialNumber`/`GetSecret` 只读 NVS/efuse 出厂值；**无值即进「未出厂配置」错误态**（不再自生成），屏显提示、不 Bootstrap。
- **待确认**（判断 3）：secret 存 NVS（量产临时）or efuse（量产正式，不可逆）；默认 NVS + §7 efuse checklist。

**A2. 出厂工装（新，`tools/` 或制造侧脚本）**
- 生成 `(sn, secret)` → 烧 NVS `identity` → 调 backend Plan 5 `open_api/device.PreregisterDevice`（org 的 API-Key，`iot` scope）写 `(sn, secret_hash, org_id, memory_space_id=sn)`。**这是新增制造步骤**，非纯固件——列清单交产线。

- **验证**：预登记过的样机 Bootstrap 成功换令牌；未预登记 sn Bootstrap 被 backend 403；NVS 无身份→设备进未配置态不乱连。

### B. HTTP 控制面客户端（新 `protocols/companion_http_control.{h,cc}`）

**B1. 公共**：一个带 `Authorization: Bearer <deviceToken>` 的 HTTP POST 帮手（复用固件 HTTP 栈）；统一 `401` 拦截 → `RefreshBootstrap()`（清令牌+重 Bootstrap+有限退避，进锁定态不无限重试，判断 6/S1）。base URL 从 Bootstrap `config.endpoints` 或 `Kconfig.lmcl` 推导（控制面 RPC 路径 `/grpc-gateway/CompanionDeviceService/<Method>`）。

**B2. `ReportDeviceStatus`（心跳）**：定时器（周期 = Bootstrap `config.heartbeatSeconds`，Plan 2 下发，默认 60s）POST 状态（`firmwareVersion`/`deviceModelCode`/`powerSource`/`batteryLevel`/`boardName`/`mac`，字段沿用旧 `status` 帧 :338-361）；**全生命周期持续**（配对期、通话期都发，F26）。

**B3. `GetPairingState`（未绑定期短轮询）**：未绑定态每 ~2s POST（backend 按 sn 限频 ≤1/s，留余量）→ 读 `{phase,pairingCode,ttlSeconds,boundUserId,message}`；`phase=PAIRING`→屏显 `pairingCode`+`ttlSeconds`；`phase=BOUND`→停轮询、触发连 `/agent/v1`（C）。

**B4. `CheckFirmwareUpdate`（OTA 拉模型）**：开机后 + 周期性（或收到运营指示）POST `{sn(令牌为准,忽略), currentVersion}` → 读 `{hasUpdate,version,packageUrl,packageHash,packageSize,forceUpdate}` → `hasUpdate` 则调**现有** `companion_ota.UpgradeFirmware(url,hash,size,version)`（OTA 下载/校验逻辑**不动**，仅触发源从 WSS `update` 帧改 HTTP 响应）。

- **验证**：设备 POST 心跳 → backend `last_report_at` 刷新、列表 online；未绑定轮询显示码、绑定后 `phase=BOUND`；OTA 判定与旧一致、SHA256 校验绿。

### C. Agent 面 WSS 帧集对齐（`companion_protocol.{h,cc}` + `application.cc` hook）

**C1. `companion_protocol` 收敛**
- 删「连后发 `status`/`checkUpdate` WSS 帧」(:338-371)——移到控制面 HTTP（B）。
- `OpenAudioChannel` 简化为「Bootstrap（若无有效令牌）→ 连 `/agent/v1?token=<deviceToken>` → 等 `ready`（原 `hello`）」；删未绑定期的 WSS `pairingCode`/`bound` 等待分支（未绑定根本不连 WS，判断 2）。
- 上行删 `abort` 帧（:384-393）——手停改**本地停播**（停渲染下行音频/字幕、`Abort` 本地播放器），不发帧（D7）。`listen` 仅 `start`（去除任何 `stop` 发送，D8）。
- WS URL/连接、二进制 `0x01` 收发、AEC/Opus 参数**不动**。

**C2. `application.cc` 帧分发 hook（Tier-3 keep-ours）**
- 服务器帧改按新契约 `{type,data}`：
  - `hello`→**`ready`**（读 `audio{format,sampleRate}`、`voiceEnabled`；据 `voiceEnabled` 显隐麦克风；**每次连按全新会话处理**，清残留 UI，D6）。
  - `sleep`→**`intent{kind:SLEEP}`**（读 proto enum，`SLEEP` 触发本地回待唤醒；`reason` 仅展示）。
  - `update`/`pairingCode`/`bound`/`status`/`checkUpdate` 帧分支**删**（走控制面）。
  - `stt`/`subtitle`/`audioStart`/`audioEnd`(→`turnEnd`)/`error`：**全部读 `turnId`**，丢弃非当前轮迟到帧；`turnEnd` 作每轮唯一终结（收到才清「正在说」、判正常/打断/失败）。
  - `error{stage,message}`：`stage` 为 enum（ASR|LLM|TTS），`message` 是 backend 脱敏固定文案，展示即可。
  - 二进制下行 `0x01`：读 `[0x01][turnId低8位][Opus24k]`，turnId 低 8 位 != 当前轮则丢（作废轮迟到音频）。
- 接 HTTP 控制面定时器起停（心跳常驻、配对轮询仅未绑定、OTA 周期）。
- **手停本地化**：按键/停止 → 本地停播 + 本地 `Abort`，**不发 `abort`**（D7）。

- **验证**：绑定后连 `/agent/v1`→收 `ready`；语音轮/文字轮正常；被打断收 `turnEnd{interrupted}` 清 UI；`intent{SLEEP}` 回待唤醒；手停仅本地停、无上行帧；作废轮迟到音频被 turnId 丢弃。

### D. 配对与令牌 UX

**D1. 配对流程**：Bootstrap 成功 → 未绑定 → 起 `GetPairingState` 轮询 → 屏显码/倒计时 → `phase=BOUND` → 停轮询 + 连 `/agent/v1`。**取代**旧 WSS `pairingCode`/`bound` 帧 + `NotifyDeviceBound` 反向回调。
**D2. 令牌续期**：控制面/Agent 面遇 `401` → `RefreshBootstrap`（B1）；Agent WS 被踢（`1008` 封禁/吊销）→ 停连、进对应态（不无限重连）。

- **验证**：全新样机 Bootstrap→显示码→手机绑定→设备自动进对话；令牌过期自动重 Bootstrap；被封禁踢断后不狂重连。

### E. 文档同步

- 固件仓 `docs/`：更新连接契约（HTTP 控制面 + `/agent/v1` 帧集），登记 `application.cc`/`companion_protocol` 的新 Tier-3 hook 到 `lmcl-upstream-sync-and-isolation.md` §8.2。
- 与 backend `docs/device-access.md` 设备侧实现对齐（双向一致）。

---

## 4. 配置与构建

- `Kconfig.lmcl`：Bootstrap URL 保留；如需可加控制面 base URL / 心跳周期默认（优先用 Bootstrap `config` 下发、Kconfig 仅兜底）。
- 分区表/OTA 不变（`partitions/v2/16m.csv`）；版本串仍 `esp_app_get_description()->version`（`ReportDeviceStatus`/`CheckFirmwareUpdate` 用）。
- 无音频参数改动（16k/24k/20ms/AEC 保持）。

---

## 5. 执行顺序与验证

1. **前置**：backend Plans 1-5 落地验证、`device-access.md` 定稿冻结契约。
2. **A**（出厂身份）→ **B**（HTTP 控制面客户端）→ **C**（Agent 帧对齐）→ **D**（UX）→ **E**（文档），全落 lmcl 隔离层 / 已登记 hook。
3. **编译**：ESP-IDF build 绿（`idf.py build`）；不碰 upstream 共享核心（`git diff upstream 共享文件` 仅已登记 hook）。
4. **硬件冒烟**（隔离纪律要求的 merge 后测试序）：预登记样机 → Bootstrap → 配对轮询显示码 → 手机绑定 → `phase=BOUND` 自连 `/agent/v1` → 语音一轮 + 文字一轮 + 打断 + `intent{SLEEP}` → 心跳在线 → `CheckFirmwareUpdate` OTA 刷写 + reboot 提交。
5. **铺装恢复设备可用**：新固件冒烟全绿 → 刷给所有测试样机（`/device/v1` 已在 Plan 3 删除，样机必须刷新固件走新契约才能连）。

---

## 6. 风险与陷阱

- **契约冻结是硬前置**：backend Plans 1-5 未定稿就改固件 = 边改边变。等 `device-access.md` 冻结。
- **`/device/v1` 已在 Plan 3 删除的时序**：本 plan 之前旧路径已断，测试样机需靠保留的旧固件构建或 backend test-client 过渡；**固件改到新契约是恢复样机可用的唯一路径**（0 上市设备使这可控）。铺装顺序：新固件冒烟绿 → 逐台刷新样机。
- **出厂预登记改制造流程**（A2）：不再自生成身份是最大工艺变更——需产线工装（生成→烧录→调预登记 API）。secret 存 NVS 可被擦（临时），量产要 efuse（§7）。
- **不手改 upstream 共享核心**：`application.cc`/`audio_service` 改动必须走**已登记 Tier-3 hook** + keep-ours，否则每次 merge upstream 打架。新逻辑尽量进 `companion_http_control`（新文件零冲突）。
- **保活 ping 刷新未验证**（`companion_protocol.cc:106-114` ⚠）：backend 契约要求 server 60s ping 刷新设备应用层保活（§3.1/F34）；固件侧需**硬件验证** WS 库是否随 server ping 刷新读超时，否则空闲闪断——列冒烟必测项。
- **turnId 丢帧逻辑**：下行全帧带 turnId，二进制取低 8 位；实现丢弃「非当前轮」迟到帧，别把当前轮也丢了（回绕边界）。
- **手停纯本地**（D7）：确保按键手停只停本地渲染 + 本地 `Abort`，**绝不发任何上行帧**；后续用户再说/打字归自然打断。
- **令牌 1h TTL**：控制面长轮询/心跳跨 1h 会 `401`；`RefreshBootstrap` 要稳（有限退避、锁定态），否则封禁设备凭出厂密码狂刷 Bootstrap（backend S1 现查 status 拦，但固件也别狂试）。
- **无摄像头**：`lmcl-box-v1` 不发 `0x02`；协议留着、本期不实现。

---

## 7. 完成定义（DoD）

- 控制面全 HTTP：`ReportDeviceStatus`（心跳常驻）/`GetPairingState`（未绑定轮询）/`CheckFirmwareUpdate`（OTA）经 `companion_http_control` 带令牌调通；WSS 不再承载 status/checkUpdate/pairingCode/bound/update。
- Bootstrap 出厂预登记：设备读出厂 sn+secret（不自生成）、backend 预登记验证通过；出厂工装脚本就绪。
- `/agent/v1` 帧对齐：仅绑定后连；`ready`/`turnEnd`/`intent{SLEEP}` 处理 + 全下行读 turnId；`abort` 上行删（手停本地）；`listen` 仅 start；音频管线（16k/24k/20ms/AEC）未动。
- 配对 UX 轮询驱动（码来自 `GetPairingState`、绑定见 `phase=BOUND` 自连）；令牌 `401` 自动重 Bootstrap；封禁踢断不狂重连。
- ESP-IDF 编译绿；upstream 共享核心仅已登记 hook（merge keep-ours）；硬件冒烟全序通过（Bootstrap→配对→对话→打断→sleep→心跳→OTA→reboot）。
- 文档：固件仓 `docs/` + backend `device-access.md` 设备侧双向一致；新 hook 登记进隔离文档。
- **未做/另案**：视觉 `0x02`（无摄像头）；push-to-talk/隐私关麦（YAGNI）；OTA 固件签名 + anti-rollback + secret 存 efuse（**上市硬门槛 checklist**，配合 backend §4.5）。

---

> 落点仓 = `D:\workspace\Xiaole_AI_Companion_Robot`（本文档实施时复制到固件仓 `docs/`）。未涉及 git 提交/推送。严守隔离纪律：新能力=新文件、共享核心走已登记 hook + keep-ours；契约冻结（Plans 1-5）后再动；`/device/v1` 已在 Plan 3 删除，固件改到新契约是恢复样机可用的唯一路径。

