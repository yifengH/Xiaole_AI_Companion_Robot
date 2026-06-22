# 黎明缠论(lmcl)设备固件 · 上游跟随 + 定制隔离方案

> 目标:把 xiaozhi-esp32 当**持续跟随的上游依赖**,白吃整个硬件生态的红利(新板子/新芯片/AEC/唤醒词/bugfix);
> 同时把我们的定制收进**隔离层**,让上游高频改动的共享文件保持与官方一致,`git merge upstream` 几乎无冲突。
> 这份是给人审阅的方案,审阅通过后再分阶段动手。

## 0. 现状与动机(数据)

- 上游 `78/xiaozhi-esp32` 已比我们的基线领先 **~240 个 PR**(upstream `#2067` vs 我们 import 前的 `#1821`)。
- 我们(同事 import + 本轮契约对齐)相对 pristine 基线 `cc7cbe7` 改了 **39 个文件**:
  - ✅ **已隔离(10 个新增文件,保留不动)**:`device_identity.*`、`alarm_manager.*`、各 board 的 `lamp_G.h`/`jidian_G.h`、`boards/common/dfplayer_controller.h`、`assets/common/alarm_ring.ogg`。这是同事做对的部分——新能力=新文件。
  - ⚠️ **焊进共享核心(16 个,本方案要抽离)**:见 §2。
  - 🔁 **删了共享文件(2 个)**:`mqtt_protocol.cc/.h`(本轮我删的)——跟随上游下应**恢复**(上游仍有它,删了每次合并都打架;不用它只要不选即可)。

## 1. 上游同步工作流(已就绪)

```bash
git remote add upstream https://github.com/78/xiaozhi-esp32.git   # 已加(只读,fetch 不需权限)
git fetch upstream                                                 # 已验证可拉
# 周期性(如每月/每次上游发版):
git fetch upstream
git switch main && git merge upstream/main      # 把上游并进我们的 main
# 隔离做到位后,绝大多数共享文件自动合;只剩极少数已知 hook 点需手解
```

要点:**我们永远只在自己的 fork 里改,上游只读拉取**。push 才要权限,fetch 不要。

## 2. 隔离目标:16 个共享核心文件 → 抽到哪里

按"能不能干净抽离"分三档。**完美零改动不可能**(定制音频/显示/启动流必然要插钩子);目标是把冲突面从"16 个大冲突"压到"几个已知小 hook"。

### Tier 1 — 干净可抽,收益最大,先做
| 共享文件(现状) | 抽离去向 | 做法 |
|---|---|---|
| `protocols/websocket_protocol.cc/.h`(+386,焊入 Bootstrap+我们的帧契约) | **新建 `protocols/companion_protocol.cc/.h`(`CompanionProtocol : Protocol`)** | 把 Bootstrap、`{type,data}` 帧、0x01 音频、sleep/error、常驻连接全部搬进新子类;`websocket_protocol.*` **还原到上游一字不差** |
| `ota.cc/.h`(我把它 gut 了) | **`CompanionProtocol` 里调用,或新建 `companion_ota.*`** | 还原 `ota.*` 接近上游;我们的"WSS update 帧 → sha256/size 校验刷写"逻辑放我们文件 |
| `protocols/protocol.cc/.h`(基类被改) | 还原上游;新增的接口放子类 | 评估每处改动,能放子类的放子类 |
| `mcp_server.cc`(+224,我们的工具焊进共享文件) | **新建 `companion_mcp_tools.cc`**,由我们的 board/app 调 `McpServer::AddTool(...)` | `mcp_server.cc` 还原上游(它是通用框架,工具本就该外部注册) |
| 删除的 `mqtt_protocol.*` | **恢复**(git revert 那次删除) | 不选 MQTT 即可,文件留着跟上游一致 |
| `CMakeLists.txt`(删了 mqtt 行) | 恢复;新增源用我们 board 目录的 glob 自动纳入 | — |

### Tier 2 — 需要"我们自己的 board 目录",依赖一个待确认项(见 §3)
| 共享文件(现状) | 抽离去向 |
|---|---|
| 就地改的 `boards/bread-compact-wifi(-lcd)/config.h`、`compact_wifi_board*.cc` | **新建 `boards/lmcl-box-v1/`**(从我们实际用的参考板复制一份+我们的板级定制),`bread-compact-*` 还原上游 |
| `Kconfig.projbuild`(+72,我们的配置项) | 我们的 `CONFIG_DEVICE_*`/`LMCL_*` 收进 board 目录自带的 Kconfig 片段 |
| board 里的 `lamp_G.h/jidian_G.h`(现加在两个参考板下) | 移到 `boards/lmcl-box-v1/` |

### Tier 3 — 深度音频/显示改动,逐项评估,可能保留为"已记录的小 hook"
| 共享文件 | 难点 / 处置 |
|---|---|
| `audio/codecs/no_audio_codec.cc/.h`(+234) | 若是我们硬件的全双工/裸 I2S 需求 → 做成我们 board 私有 codec 类,还原共享 codec;**需先读懂这 +234 改了什么** |
| `audio/audio_service.cc/.h`(+98) | 评估哪些是通用、哪些我们专属;通用的争取并回上游,专属的下沉 |
| `audio/processors/afe_audio_processor.cc`、`wake_words/afe_wake_word.cc`(各 +2) | 改动极小,大概率可并回上游或保留为已知 1~2 行 hook |
| `display/*`(display/lcd/oled/lvgl + .h) | 配对码/字幕 UI。部分是必要显示 API。能下沉的下沉,剩余记为已知 hook |
| `application.cc`(±324)、`application.h` | **不可能完全零改**:启动流要插我们的初始化、常驻连接、帧分发。目标=收敛成**最少的几个 hook 调用**(调我们的模块),主体逻辑搬出去 |

## 3. 待确认的唯一阻塞项(Tier 2 需要)

**我们的物理设备实际对应哪个参考板?** 证据矛盾:`sdkconfig.defaults` 没钉 `BOARD_TYPE`(→ Kconfig 默认 `bread-compact-wifi`,无屏),但同事同时改了 `bread-compact-wifi-lcd`(有屏),且"陪伴机器人"通常带屏。
- 建这个 `boards/lmcl-box-v1/` 必须照实际硬件的 GPIO/codec/屏来,否则烧了点不亮。
- **请确认**:量产/样机实际烧的是哪一款(或给我硬件原理图/引脚表)。Tier 1 不依赖这个,可先做。

## 4. 不做什么(YAGNI,且尊重你"留期权"的判断)

- **不删用不到的板子/4G/配网/MCP**:它们是上游红利的载体,跟随上游下保留=零成本、未来换硬件直接吃。删了反而每次合并要打架。
- **不把"事件组+单主循环"等 MCU 并发模型搬进 Go 后端**:场景前提不同,后端有真并发,搬过去是退化。

## 5. 分阶段执行 + 验收

每阶段一组提交,**单独可审、可回滚**;每阶段后 `git merge upstream/main` 跑一遍,量化冲突面是否下降。
- 阶段 A(Tier 1)✅ 已完成:协议/OTA/MCP 工具抽离 + 恢复 MQTT。**最大收益,无硬件依赖**。
- 阶段 B(Tier 2)✅ 已完成:建 `boards/lmcl-box-v1/`,bread 参考板还原上游。
- 阶段 C(Tier 3)✅ 已审计:音频/显示逐项评估,结论=**保留为已记录 hook**(理由见 §8)。
- **验收**:本机无法编译 ESP-IDF;须你在硬件上 build+烧录验证。重点回归:Bootstrap→配对→连麦→打断→OTA→常驻重连 + 闹钟 + 点屏。

## 6. 风险

- 全程在**未编译验证**下改固件(本机无 ESP-IDF),靠静态推理 + 你上机验收。故分小阶段、每步可回滚。
- 抽 `CompanionProtocol` 时要确保把契约对齐的全部行为(本轮 2 个提交的成果)无遗漏地搬过去——会对照清单逐条核。

## 7. 团队协作流程(谁做、怎么做、什么时候做)

### 7.1 upstream 远程要不要每人自动配?——不要,且没必要

- `git remote add upstream` 写在各自的 `.git/config` 里,**不随仓库提交**,所以别人 clone 不会自动有它。git 也**没有"clone 时自动加远程"的机制**(clone 不触发可分发的钩子)。
- 但这其实是**伪需求**:**同步上游是低频、敏感、该由一个人负责的操作**,日常开发的同事**根本不需要 upstream 远程**。他们只在我们自己的仓库上正常 `pull main` / `push 功能分支`。
- 结论:**设一个"上游同步负责人"**(固定或轮值),只有他配 upstream(一条命令,或跑下面的脚本)。**不搞全员自动化**——那是给不需要的人解决不存在的问题。

### 7.2 分支模型(就三条线)

```
upstream/main   ← 官方只读镜像(fetch 来,谁都不在上面提交)
main            ← 我们的产品线 = 上游 ⊕ 我们的隔离层。所有人基于它开发/构建
功能分支 feat/* ← 各同事日常在这开,PR 回 main
```
> 「`main` 里既有我们的改动又有官方改动」——**这正是期望状态**,不是问题。`main` 的定义就是"上游 + 我们的隔离层",合并节点就是两者的缝。

### 7.3 同步上游的"圆"怎么处理:用 merge,别用 rebase

你说的"两个圆"= 历史分叉后合并产生的**带两个父的合并提交**。处理方式两派:

- ✅ **merge(`git merge upstream/main`)**:产生一个合并"圆",**非破坏性、不改写已有提交哈希**。对**共享分支(多人基于 main)是唯一安全选择**。冲突解一次,记录在这个合并提交里。**我们用这个。**
- ❌ **rebase**:把我们的提交"重放"到上游之上,历史变直、没有"圆",看着干净——**但它改写提交哈希**,在多人共享的 main 上会让**所有同事的本地历史对不上、被迫强推**,是灾难。只适合"私有、随时可丢弃的小补丁串",不适合我们。

**所以:别嫌那些合并"圆"难看——它们是"我们在这个时间点拉了上游"的诚实、安全的记录。** 追求直线历史在共享 fork 上是因小失大。

### 7.4 一次同步的可执行流程(负责人做)

**绝不直接在 main 上 merge 上游**——先在临时分支做、测过、PR 评审后再进 main:

```bash
# 0) 一次性(仅负责人):git remote add upstream https://github.com/78/xiaozhi-esp32.git
# 1) 起同步分支(脚本 scripts/sync-upstream.sh 把 1~3 一把做了)
git fetch upstream
git switch -c sync/upstream-2026-07 main
git merge upstream/main            # 这里产生合并"圆";有冲突就解(见 7.5)
# 2) 本地 build + 烧录 + 冒烟测(Bootstrap→配对→连麦→打断→OTA→重连)
# 3) 推分支、开 PR: sync/upstream-2026-07 → main,由另一位同事 review(重点看解冲突处)
# 4) 合 PR。此后所有人 `git pull main` 即拿到最新上游 + 我们的改动
```

### 7.5 解冲突(隔离做到位后,这是 5 分钟的活)

- 隔离后绝大多数文件**自动合**;冲突只出现在我们少数"已知 hook 点"(如 `application.cc` 的初始化挂钩、`Kconfig`/`CMakeLists`)。
- 出冲突时:`git status` 看冲突文件 → 打开看 `<<<<<<< HEAD(我们) / ======= / >>>>>>> upstream/main(上游)` 三段 → **保留双方意图**(我们的 hook + 上游的新代码)→ `git add 文件` → `git merge --continue`。
- 隔离重构(§2)的全部意义,就是把这一步从"16 个大冲突、根本合不动"压到"几个小冲突、几分钟搞定"。

### 7.6 日常同事视角:只有一条干净的线

普通同事**永远不碰 upstream**,只 `git pull main` + 在 `feat/*` 上开发 + PR 回 main。负责人合好的上游早已"烘焙"进 main,他们看到的就是一条正常推进的 main——"两个圆"只在负责人那次同步操作里短暂存在,对日常开发透明。

### 7.7 频率与时机

- **频率低**:按需即可——上游发版(打 tag)、需要某个上游 bugfix/新板子、或固定每月一次。**不是每天**。
- **时机**:挑 main 相对安静时做(进行中的大功能少),解冲突更省心。merge 非破坏性,**不需要冻结开发**。
- **当前历史已就绪**:同事的 import 是"叠在上游提交 `cc7cbe7` 之上"(不是压扁的),所以 git 能找到共同祖先做三方合并,第一次 `git merge upstream/main` 机制上没问题。

## 8. 执行结果与「已知 hook 登记」(Tier1–3 完成)

### 8.1 已彻底隔离(抽进我方独立文件,与上游零冲突)

这些是**新增文件**(`git diff upstream --diff-filter=A`),上游没有、永不冲突;我方接入逻辑全在这里:

| 文件 | 内容 |
|---|---|
| `protocols/companion_protocol.{h,cc}` | `CompanionProtocol`:Bootstrap+{type,data}帧+0x01音频+常驻连接+300s超时 |
| `companion_ota.{h,cc}` | `CompanionOta`:WSS update 帧驱动的 sha256/size 校验刷写 |
| `companion_mcp_tools.{h,cc}` | 闹钟等设备能力 MCP 工具(外部 `AddTool` 注册) |
| `device_identity.{h,cc}` | 设备 sn/secret(NVS 高熵随机,非 MAC 派生) |
| `alarm_manager.{h,cc}` | 闹钟管理 |
| `boards/lmcl-box-v1/*` | 我方板:屏/引脚/继电器/LED/DFPlayer |
| `Kconfig.lmcl` | 我方追加配置(DEVICE_*/LMCL_*/Alarm),projbuild 仅一行 rsource |

对应的上游共享文件已 **`git checkout upstream` 字节还原**:`protocol.cc`、`websocket_protocol.*`、`mqtt_protocol.*`、`ota.*`、`mcp_server.cc`、`bread-compact-wifi(-lcd)/*`、`boards/common/`。

### 8.2 已知 hook 登记(剩余 diverge 的共享文件 = 合并时 **keep-ours**)

下列文件仍与上游有差异,但都是**有意的、嵌进上游类生命周期/管线的就地修改**,**无法在不重写时序/声学敏感代码的前提下干净抽离**。本机无 ESP-IDF、无硬件,强行抽离=镀金且有真实回归风险(违背「激进度按可逆性分配」)。故**决定保留在原处**,在此登记;`git merge upstream` 时这些文件冲突一律 **keep-ours + 人工核对上游是否在同段有新逻辑**:

| 文件 | 我方改动 | 为何不抽 |
|---|---|---|
| `application.cc/.h` | 帧分发(tts/stt/sleep/update/error/pairingCode)、常驻连接重连、状态上报、唤醒流 | 这**就是**我们的应用主逻辑,非"可外置能力";上游 application 是骨架,我们的业务必然在此 |
| `audio/audio_service.{cc,h}` | `OPUS_FRAME_DURATION_MS 60→20`(20ms 帧契约)、队列加大、ducking/逐包增益、PlaySound 扩参 | 修改上游音频管线内部,非新增;时序敏感 |
| `audio/codecs/no_audio_codec.{cc,h}` | 设备端 AEC 软件参考(Write/Read 改造、output_buffer/slice/ref_mutex) | 改 codec 读写内部,**声学关键**,改错=回声消除失效,只能上机验 |
| `audio/processors/afe_audio_processor.cc`、`wake_words/afe_wake_word.cc` | 各 1~2 行 | 极小,抽离不值 |
| `display/{display,lcd_display,oled_display}.{cc,h}`、`lvgl_display/lvgl_display.{cc,h}` | 闹钟全屏覆盖层 + 状态栏闹钟图标 | 嵌进 LvglDisplay 构造/析构/UpdateStatusBar **生命周期**;独立成类需插继承链,风险高 |
| `Kconfig.projbuild` | 板子选项条目 + LCD depends 加我方板 + rsource 钩子;`USE_DEVICE_AEC` 改 default y/去白名单 + 新增 `USE_REALTIME_CHAT` | 选项条目/depends 必须在上游 choice 内;AEC 收敛涉及 `USE_AUDIO_PROCESSOR` 依赖子交互,无硬件不敢动 |
| `protocol.h` | 2 处:AudioStreamPacket 的 ducking/gain 字段、空默认虚函数 `SendDeviceStatus()` | 已是最小 hook(+7 行) |
| `CMakeLists.txt` | SOURCES 换我方文件 + lmcl-box-v1 板分支 | 构建清单,必然差异 |

> **后续可选深抽**(需硬件在手再做,否则不划算):① `no_audio_codec` 的设备 AEC 改造做成 `boards/lmcl-box-v1/` 私有 codec 子类,还原共享 codec;② 显示闹钟覆盖层用「LvglDisplay 持有一个 AlarmOverlay 组合对象」抽出;③ `USE_DEVICE_AEC` 还原上游白名单结构 + 把 `BOARD_TYPE_LMCL_BOX_V1` 加进去 + `default y if` 我方板。**三项都需上机验证 AEC/显示,当前阶段刻意不做。**

### 8.3 冲突面量化(三阶段成效)

| 节点 | 与上游 diverge 的共享文件 | 行数 |
|---|---|---|
| sync 后(重构前) | 27 | +1394 / -818 |
| Tier1+2+3 后 | 17 | +688 / -255 |

协议/OTA/MCP/参考板**四大冲突源已归零**;剩余 17 个均为 §8.2 登记在案的有意 hook,合并可控。
