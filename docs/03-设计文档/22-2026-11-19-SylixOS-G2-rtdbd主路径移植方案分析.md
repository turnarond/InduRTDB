# SylixOS G2 · rtdbd 主路径移植方案分析（SDD 第一步）

> **状态**：v2（两轮独立评审通过 + 用户 h-A~h-D 裁决落痕，见 §6.1）。纯文档、`tdd:免`，不含产品代码、不构成开工授权。
> **触发**：用户 2026-11-19「先推进 G2」；上游 21 号方案 §3.2/§3.3、q-B 裁决（SylixOS 上 UDS 写裁撤、身份走 VSOA hello）；G0/G1 与 atomic64 修复已闭环（commit c89e5df/5114078，单台板上写路径 PASS）。
> **G2 目标（h-A 裁决收窄后）**：在单台 SylixOS/ARM64 设备上让 rtdbd 跑通 **VSOA loopback 写入 + 定向 NOTIFY 最短闭环**（含 CMake 接入、legacy UDS/supervisor 裁剪、SDK 符号/运行硬门），Linux 侧行为零变化；**订阅重建经 h-A 裁决排除、另立变更卡**。
> **边界**：本文是平台适配的 SDD 第一步，**不改** UDS opcode/ABI（13 号 §5 冻结）、不改对外平台口径、不填 B 段数字、发布门仍 FAIL 8；订阅重建/审计-health/网关三项实现缺口经 h-B/h-C 裁决均另立变更卡。

---

## 0 结论摘要

**技术上可行，G2 的主要工作是"构建接入 + 条件编译裁剪"，而非重写协议栈**：VSOA 的写/dispatch/鉴权/通知门控是平台无关纯逻辑（Linux 现场已端到端验证），SDK 在 SylixOS 有 11 架构预编译库；障碍是 ① CMake 未让 `rtdbd`/VSOA 在 SylixOS 下构建、② rtdbd 中 UDS/supervisor 的 Linux 专有代码无条件编入、③ 两个**未落地实现缺口**（订阅重建、审计/health）需在 G2 明确取舍。**SDK 符号/ABI 已机械确认**（见 §2.4：11 个 server + 3 个 client 符号交叉 nm 全导出、版本 1.9.1），但 **`vsoa_server_start` 的 loopback bind、`cli_datagram` 收发在 SylixOS 上从未实测**（G0/G1 只跑核心库 shm/原子/pthread）——符号存在 ≠ socket 可用，该项列为 G2.3 硬门。

| 层次 | 判定 | 主要障碍 | 优先级 |
|---|---|---|---|
| VSOA SDK（libvsoa 1.9.1） | ✅ 11 架构预编译库含 aarch64；**交叉 nm 实测 11 server + 3 client 符号全导出、版本 1.9.1**（符号/ABI 已确认） | VSOA socket 在 SylixOS 的 bind/收发**未实测**（G2.3 硬门）；不得把符号可用外推为运行可用 | 符号门已过；运行门待 G2.3 |
| CMake 接入 | ⚠️ VSOA 探测块与 rtdbd target 均被 Linux guard 排除 | 平台化探测 + `check_vsoa_sdk.py` 参数化 + rtdbd 条件构建 | P0 |
| rtdbd 双通道裁剪 | ⚠️ 写主路径平台无关；UDS/supervisor 无条件编入 | `#ifdef INDURTDB_OS_LINUX` 隔离 UDS listener/peer_cred/supervisor | P1 |
| 订阅重建 | ❌ 未落地（`defer_on_rebuild_subs` 只清空、断链未入队） | 实现缺口，非平台障碍；**h-A 裁决排除出 G2、另立变更卡**（§6.1） | 范围外 |
| 审计 / health 镜像 URL | ⚠️ 生产未注入审计回调、`/health` 恒 NOT_IMPLEMENTED | 实现缺口，默认 VSOA 最短路径不依赖；**h-B 裁决排除、另立**（§6.1） | 范围外 |

**一句话**：G2 能跑通"VSOA 写入 + 定向 NOTIFY"的最小闭环；订阅重建若要真实恢复（而非清空）需补实现，经 h-A 裁决已与 G2 构建移植解耦、另立变更卡分切片推进。

---

## 1 目标与范围

### 1.1 G2 在本路线中的位置

G0（交叉编译打通）→ G1（板级基础验证，发现并修复 atomic64 崩溃）→ **G2（rtdbd 主路径移植，本文）** → G3（集群语义与时钟）→ G4（现场 F2/F3a/F4 取证）。G2 前置：G1 已通过（单台写路径 PASS）；SylixOS 无 fork、RTC=2000-01-01 为已知平台事实（G1 实施报告）。

### 1.2 范围内 / 范围外

- **范围内**：CMake 让 rtdbd+VSOA 在 SylixOS 构建；rtdbd UDS/supervisor 条件化；VSOA 写入+定向 NOTIFY 端到端最小 harness；SDK 符号/ABI 门。
- **范围外（另立变更卡）**：订阅真实重建（断链入队 + service_id 订阅快照重放）；VSOA 审计回调注入；`/health` 等镜像 URL 实现；client/监控台的 VSOA 网关；跨机集群语义（G3）。

> **与上游 G2 范围的定义差异（评审 P1-2，已由 h-A 裁决闭环）**：21 号 §5:171 原定 G2 退出判据为「板上 VSOA 写入 + 定向 NOTIFY + **订阅重建**端到端通过」。用户 2026-11-19 裁决 h-A=否（见 §6.1），**G2 收窄**为「写入 + 定向 NOTIFY 最短闭环」，订阅真实重建（未落地实现缺口，见 §2.5）另立变更卡；21 号 §5:171 已回填为收窄后判据，两处一致。

---

## 2 现状盘点（只读调研，带证据）

### 2.1 双通道结构：写主路径已共用且平台无关

rtdbd 有 UDS 与 VSOA 两条前端写路径，共用同一套业务层：

| 环节 | UDS 路径 | VSOA 路径 | 共用？ |
|---|---|---|---|
| 鉴权+落值 | `op_write`→`write_one`→`rtdbd_dispatch.c:98-139` | `vsoa_on_cmd:469`→`rtdbd_dispatch.c:98-139`（`vsoa_adapter.c:486`） | ✅ 同一函数 |
| 订阅鉴权/读点/notify 帧/RBE/URL↔opcode | `rtdbd.c` 各 op | `rtdbd_dispatch.c:141-267,20-51,190-200` | ✅ 同一函数 |
| 通知投递 | `notify_broadcast`+outq（`rtdbd.c:608`） | `vsoa_notify_dispatch`→`vsoa_server_cli_datagram`（`vsoa_adapter.c:225-263`） | ❌ 传输特有 |
| 会话类型 | `rtdbd_conn_t.session` | `a->cli[].session` | ✅ 同一 `rtdbd_session_t` |

**关键结论**：VSOA 写路径不触碰 `peer_cred`/`getsockopt`/`g_conns`/`notify_broadcast`/outq（`vsoa_adapter.c:469-498`），共享依赖只有 `g_h`、`policy/conf`、`rtdbd_peers_global()`——均平台无关。裁掉 UDS 后写主路径可独立工作。

### 2.2 SO_PEERCRED：唯一真实调用点，已被 VSOA 契约替代

- `getsockopt(SO_PEERCRED)` 唯一真实调用在 `peer_cred()`（`rtdbd.c:795-802`），**唯一调用点是 UDS accept 路径 `rtdbd.c:2770`**。按点鉴权（`op_write:1028`）、审计（`write_one:1013`）、SET_PEERS（`:1760`）用的是 `conn_peer_cred()` 缓存（`:804-810`）而非直接 getsockopt——随 UDS 命令表一并裁剪即可。
- VSOA 侧已替代：hello 握手（token 查表 + service_id 一致性 + 来源 IP 纯函数校验，`vsoa_adapter.c:130-194`）写入 `sess->cred.uid/role/partition`；写路径只吃 uid（`vsoa_adapter.c:486-487`），无 `getsockopt`；SET_PEERS 有 VSOA 镜像 `/sync/peers/set|get`（`:282-313`）复用同一 `rtdbd_peers_apply_set`。
- `rtdbd_dispatch.h:95,114` 明确"凭据来源属前端：UDS=SO_PEERCRED；VSOA=握手绑定"。**q-B 契约层已成立**。
- 隔离手段：`peer_cred` 与 UDS listener 一起用 `INDURTDB_OS_LINUX` 包住；Linux 默认开启，行为零变化。

### 2.3 supervisor（-S）：运行时可选但无条件编入

`fork`/`execv("/proc/self/exe")`/`waitpid`/`kill` 集中在 `rtdbd.c:1908-2050`（terminate_and_reap + supervise_loop），唯一调用点 `:2387-2389`，`supervise` 默认 false（`:2215`）。**无 `#ifdef`，SylixOS 交叉编译仍会因 fork/waitpid 符号失败**。隔离：把 `:1889-2050` 与调用点包进 `#ifdef INDURTDB_OS_LINUX`。需条件化的 Linux 专有头不止 `sys/wait.h`（`:19`）：`sys/un.h`（`:18`，AF_UNIX）、`getopt.h`（`:10`，`getopt_long` 用于 `:2255`）、`poll.h`（`:11`）、`unistd.h`（`:21`）同属 UDS/supervisor 隔离面；`getopt_long` 在 SylixOS 的可用性列为 G2.1 实测项（P2-3）。复用已定义的 `INDURTDB_OS_LINUX`/`INDURTDB_OS_SYLIXOS`（`CMakeLists.txt:17/20`），**无需新增 `INDURTDB_ENABLE_UDS` 宏**（P2-7）。

### 2.4 CMake 接入障碍

| 位置 | 现状 | 需改 |
|---|---|---|
| `CMakeLists.txt:68` | VSOA 探测块 `if(ENABLE_VSOA AND Linux)` → SylixOS 下静默无效 | 放宽到 `Linux OR SylixOS` |
| `:66-67` | `VSOA_SDK_ROOT` 描述/`VSOA_LIB_SUBDIR` 默认 `lib/linux/x86_64` | 平台化：SylixOS → `lib/sylixos/aarch64` |
| `:76-83` | 探测只传 `--root --json`，未传 lib-subdir | 追加 `--lib-subdir`、`--check-symbols --nm <交叉nm>` |
| `:109-119` | rtdbd 只在 Linux `add_subdirectory(rtdbd)` | SylixOS 顶层可开 `INDURTDB_BUILD_RTDBD`；client/diag/tests 继续 Linux-only |
| `rtdbd/CMakeLists.txt:58-60` | `--disable-new-dtags`、`BUILD_RPATH` 指向构建机路径 | 加平台守卫；SylixOS 用部署期库路径或确认 toolchain rpath |
| `scripts/check_vsoa_sdk.py:18,55,76-82` | 硬编码 `lib/linux/x86_64`、宿主 `nm -D`、无平台参数 | 加 `--lib-subdir`/`--nm`；补 `vsoa_server_cli_address`/`vsoa_server_fds` 符号与 parser/json 库 |

**G2.0 SDK 符号/ABI 门：已实测通过（2026-11-19）**。SDK 根 = `/home/yanchaodong/work/acoinfo/vsoa_framework`（21 号/CODEBUDDY.md 冻结的 libvsoa 1.9.1 路径；另有 `eao_framework` 同尺寸快照，本文证据均以 `vsoa_framework` 为准）。用 `aarch64-sylixos-elf-nm -D` 对 `$SDK/lib/sylixos/aarch64/` 实测，**覆盖 adapter 与 harness 实际调用全集**：

- `libvsoa-server.so` 11 符号全导出：`vsoa_server_create`(e290)、`on_cli`(ec00)、`add_listener`(ef14)、`close`(e914)、`start`(e480)、`cli_address`(f410)、`cli_datagram`(101e4)、`cli_reply`(f500)、`cli_set_authed`(fcb4)、`fds`(10210)、`input_fds`(10360)。
- `libvsoa-client.so` 3 符号全导出：`vsoa_client_connect`(e284)、`call`(f440)、`input_fds`(e6a0)。
- 四库均存在：`libvsoa-server.so`(333776B)/`client.so`(329408B)/`parser.so`(9072B)/`json.so`(152896B)；`include/libvsoa/vsoa_version.h:17` 为 `VSOA_LIB_VERSION "1.9.1"`。

**结论强度边界（评审 P0 修正）**：以上是**符号/ABI 机械确认**，G2.0 可标记完成；但 G0/G1 只在板上跑了 `sylixos_smoke`（核心库 shm/原子/pthread），**从未在 SylixOS 上执行 `vsoa_server_start` 的 loopback bind 与 `cli_datagram` 收发**。符号存在 ≠ socket 在 SylixOS 可用——该项列为 **G2.3 运行硬门**，当前未验，不得外推为"SDK 侧无移植障碍/g2 已可运行"。在 `check_vsoa_sdk.py` 参数化后把上述 11+3 符号检查固化为 CMake 配置期 FATAL 门（缺任一符号即停），防 SDK 升级回归。

### 2.5 未落地实现缺口（非平台障碍，但影响端到端）

1. **订阅重建**：延迟队列本体/执行器已实现（`rtdbd_session.c:179-235`、`rtdbd_defer_actions.c:58-92`），但 `RECONNECT/REBUILD_SUBS` 无生产入队点；`defer_on_rebuild_subs`（`rtdbd.c:2131-2140`）只 `install_subs(NULL,0)`（清空订阅）；VSOA 断链（`vsoa_adapter.c:92-123`）释放会话后重连只回填 `default_uid`，不按 service_id 恢复订阅。
2. **审计回调**：生产 main（`rtdbd.c:2634-2677`）未注入 `rtdbd_vsoa_adapter_set_audit`，仅测试调用 → VSOA 写成功但不落审计。
3. **镜像 URL**：`rtdbd_dispatch.c:297-314` 只实现 PING，`/health`、`/snapshot_done`、`/notify` 等经镜像 URL 返回 `NOT_IMPLEMENTED`。`vsoa_adapter.h:70-74` 明言 `snapshot_done_caps_only` 生产恒 0。
4. **过时注释**：`vsoa_adapter.c:469-472` 仍写"变更通知尚未实现"，但 `:494-496` 已接入定向 NOTIFY。

这些是 Linux ON 构建下同样存在的行为缺口（SylixOS 无关），G2 若追求"完整订阅重建"须作为独立行为变更另立变更卡。

### 2.6 client / 监控台的已知限制

- client（`client/src/client.c:167-190`）与监控台（`rtdb_client.py:229-232`）强依赖 UDS；SylixOS 裁 UDS 后链路断，属 q-B 已知限制，非 bug。
- 最小建议：**不新增 opcode、不改 ABI**；在外部 PC 加薄 VSOA↔UDS 网关（对外暴露本地 UDS socket、对内走 VSOA `/indurtdb/v1/*`），现有监控台零改动。此网关不在 G2 范围，另立。
- Linux 侧零变化保证：q-B 裁剪只落 `INDURTDB_OS_SYLIXOS` 分支；`protocol.h` 静态断言两平台编译期都跑；既有 ctest 全绿为回归基线。

---

## 3 可行性分层结论

### 3.1 SDK：可行，符号硬门已过

libvsoa 1.9.1 `lib/sylixos/aarch64/` 库齐备、公开 API 声明与 Linux 一致、平台差异收在头文件宏（`vsoa_platform.h:59-73,273-276`）。**符号/ABI 门已实测通过**：adapter 实际调用的 11 个 server 符号与 harness 需用的 3 个 client 符号全部导出（证据见 §2.4）。但这是机械确认——`vsoa_server_start` 的 loopback bind 与 `cli_datagram` 收发在 SylixOS 上**从未运行验证**（G0/G1 只跑核心库），列 G2.3 运行硬门，**不把符号可用外推为运行可用**。

### 3.2 CMake：改造量清晰，属配置层

按 §2.4 清单改造，不涉及业务代码。风险点：`check_vsoa_sdk.py` 参数化须保持 Linux 默认行为（现有 Linux 门禁不能退化）；交叉 nm 需从 toolchain 派生而非硬编码。

### 3.3 rtdbd 裁剪：主路径可保，条件编译隔离 legacy

建议三档：
1. **必保**（平台无关，直接复用）：VSOA loopback 写入、dispatch/session/authz/defer/peers 纯逻辑、定向 NOTIFY、fd 合并、主循环 drain。
2. **条件编译隔离**（SylixOS 关、Linux 开）：UDS listener/accept/peer_cred/conn pool/outq、supervisor（`/proc/self/exe`+fork）、UDS 命令表管控 op。用 `INDURTDB_OS_LINUX` 或新增 `INDURTDB_ENABLE_UDS`，Linux 默认保持开启。
3. **不在 G2 范围**（缺口，另立）：订阅真实重建、审计注入、health 镜像 URL、VSOA↔UDS 网关。

### 3.4 端到端验证：新建 SylixOS harness

仓内 VSOA e2e 测试（`tests/integration/test_vsoa_*.cpp`）依赖 GTest/C++17/fork 拓扑、且 SylixOS 下 ctest 不注册——**断言与拓扑可复用为规格，二进制不可直接复用**。须新建 SylixOS-only harness（仿 `sylixos_smoke` 注册范式，`CMakeLists.txt:158-163`），组件：① rtdbd（VSOA ON、loopback、token 表）② VSOA 写入方（hello→point/write，24B POD）③ VSOA 订阅者（/notify datagram 回调）④ 事件循环驱动（`vsoa_server_input_fds`，T0 已证单进程同步 connect 超时）。注意 SylixOS 无 fork：写入方/订阅者须做成独立可执行或用 libvsoa client API 在同一进程内以 fd 驱动模拟。

---

## 4 与 B 段现场条件 / 发布门的关系

1. **G2 不产生 B 段真值**：单台 VSOA 写入+NOTIFY 端到端是平台适配验证，不是 F2（6 节点）/F3a（网络注入）/F4（整机重启）取证；不填 B 段 `fields`。
2. **发布门维持 FAIL 8**：不改台账/快照/校验器，不调用 `release_gate.py`。
3. **对外平台口径不改**：`07-部署运维/README.md:71` 仍「未验证、不支持生产部署」，直到完整板测与门禁全绿后统一修订。
4. **q-B 契约落地范围**：本方案只实现 SylixOS 侧 UDS 写裁撤的条件编译；Linux 侧 opcode/ABI/UDS 行为零变化。

---

## 5 分阶段路径（两轮评审通过，按裁决范围执行）

| 阶段 | 目标 | 关键动作 | 退出判据 |
|---|---|---|---|
| G2.0 SDK 符号/ABI 门 | 确认库符号与版本 | 交叉 nm 对比 11 server + 3 client 符号 + parser/json 库 + 版本 | ✅ **2026-11-19 已实测通过**（§2.4）；仍固化为 CMake 配置期硬门 |
| G2.1 CMake 接入 | rtdbd+VSOA 可交叉构建 | 按 §2.4 改造 + check_vsoa_sdk 参数化（固化 G2.0 符号检查为 FATAL 门）；实测 getopt_long/rpath | SylixOS 下 `cmake` 探测通过、rtdbd target 生成、零警告交叉编译出 rtdbd |
| G2.2 legacy 裁剪 | rtdbd 可在板上启动 | `#ifdef INDURTDB_OS_LINUX` 隔离 UDS/supervisor + 5 个 Linux 专有头 | 板上启动 rtdbd（VSOA ON）成功、无 UDS/Fork/getopt 符号错误 |
| G2.3 端到端（含 VSOA socket 运行硬门） | VSOA 写入+NOTIFY | 新建 SylixOS harness（hello→write→订阅收 NOTIFY）；**实测 `vsoa_server_start` loopback bind + `cli_datagram` 收发** | 单台板上端到端 PASS、退出码 0；定向投递量化：仅订阅者收到 NOTIFY、非订阅者/sync_peer 未收、`notify_counters` 的 `peer_notify_leak`=0（`vsoa_adapter.h:67-75`） |
| G2.4（可选，另立） | 订阅重建/审计/网关 | 独立行为变更 SDD | 按各自变更卡验收 |

每阶段涉及行为/契约变更须变更卡 + 冻结基线 + 独立评审；G2.2 的条件编译隔离与 G2.3 的新 harness 属实现，须开工授权。

---

## 6 风险与开放问题

| 类别 | 风险/问题 | 处置 |
|---|---|---|
| SDK 符号/运行 | 11 server + 3 client 符号已实测导出（§2.4），但 VSOA socket 在 SylixOS 的 bind/收发从未实测（G0/G1 只跑核心库） | 符号检查固化为 CMake 配置期 FATAL 门防回归；**socket 运行列为 G2.3 硬门，不外推符号可用为运行可用** |
| 订阅重建 | 现有只清空订阅、断链未入队，非平台障碍 | **h-A 裁决排除出 G2、另立变更卡**（§6.1） |
| 审计/health | 生产未注入审计回调、镜像 URL 恒 NOT_IMPLEMENTED | 默认 VSOA 最短路径不依赖；**h-B 裁决排除、登记为已知留白另立**（§6.1） |
| 监控台/client | 裁 UDS 后链路断 | 已知限制，VSOA↔UDS 网关另立；不新增 opcode |
| 工具链 rpath / getopt | GNU ld 专属选项、构建机路径在设备端无效；`getopt_long` 在 SylixOS 可用性未证 | G2.1 平台守卫 + 实测，不默认接受 |
| 时钟/无 fork | RTC=2000、无 fork | 用单调钟排序；harness 避免 fork（同 G1）。时间戳污染推理：写请求 `source_ts_ns` 客户端传入（`vsoa_adapter.c:484`）、NOTIFY `timestamp_ns` 取点位单调钟（`rtdbd_dispatch.c:177,183`→`rtdbd.c:161 CLOCK_MONOTONIC`）、审计 ts 前端传 0（`:491`）——**RTC=2000 不污染排序**；G2.3 校验时间戳据此设计 |
| 对外口径 | 平台支持表「未验证」 | 全绿前不改 |

### 6.1 用户裁决落痕（2026-11-19，四项均按推荐值确认）

| # | 问题 | 裁决结论 | 执行边界 |
|---|---|---|---|
| h-A | 订阅真实重建是否纳入 G2 | **否，另立变更卡** | G2 只验「写入 + 定向 NOTIFY」最短闭环；**回填 21 号 §5:171 为收窄后判据**（去掉订阅重建），实现另立 |
| h-B | VSOA 审计回调 + `/health` 镜像 URL 是否纳入 G2 | **否，登记为已知留白** | 最短路径不依赖；另立变更卡 |
| h-C | VSOA↔UDS 网关（保监控台）是否纳入 G2 | **否，另立** | G2 不碰 client/monitor；网关另立变更卡；不新增 opcode、不改 ABI |
| h-D | 本次 SDD 第一步成果如何落库/是否立即实现 | **聚合提交、暂不实现** | 22 号方案 + 评审证据 + 索引合成 1 commit（不推送）；实现按裁决范围另立变更卡 + 冻结基线 + 独立评审 + 开工授权 |

**边界**：本轮裁决确定 G2 范围为「VSOA 写入 + 定向 NOTIFY 最短闭环 + CMake 接入 + legacy 裁剪 + SDK 符号/运行硬门」，订阅重建/审计-health/网关三项除外；本文仍不是开工授权，实际实现待相应变更卡与评审完成后执行。

---

## 7 验收判据（本文作为方案文档退出门）

1. 每个成熟度判定有源码/SDK/文档行号证据，可机械复核。
2. 明确"VSOA 主路径平台无关、障碍在构建+条件编译"的结论，并给出双通道共用/不共用的证据。
3. SDK 符号/ABI 门已实测通过（§2.4，11 server + 3 client 符号全导出 + 版本 1.9.1），证据可机械复核；明确**区分**符号确认与运行未验——`vsoa_server_start` loopback bind/`cli_datagram` 收发列为 G2.3 运行硬门，不外推为"SDK 无移植障碍"；交叉 nm 检查固化为 CMake 配置期 FATAL 门防回归。
4. 本文 G2 范围（写入+NOTIFY）与 21 号 §5:171 原判据（含订阅重建）的差异已经 h-A 裁决闭环（§6.1：收窄、另立订阅重建），21 号 §5:171 已回填，两处一致。
5. 明确订阅重建/审计/health 是未落地实现缺口（非平台障碍），给出纳入/排除建议供裁决。
6. 明确 q-B 裁剪只落 SylixOS、Linux opcode/ABI/UDS 零变化、发布门 FAIL 8 零回归。
7. 分阶段路径每阶段进入前置与退出判据可证伪；契约变更点单独标注。
8. 独立方案评审通过（fresh context），评审证据落 `reviews/` 并通过 `check_review_evidence.py --require-second`。

## 8 本轮明确不做

- 不改生产代码、不引入工具链文件、不改 CMake、不连接设备。
- 不新增 opcode、不改 ABI/共享内存布局、不改对外平台口径。
- 不改发布门判定、不填 B 段数字、不打 tag、不合入 main。
- 本文记录用户已裁决的 h-A～h-D 结果（§6.1），不新增裁决；实现另起变更卡与 SDD。
