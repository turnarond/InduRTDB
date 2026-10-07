# Changelog

All notable changes to InduRTDB.

---

## [3.7.0] — 2026-10-06 / 10-07「核心语义深化 + 运维硬化」

本版本含两个主题：
- **主题A 死区/RBE + EURange 生效**（设计见 `docs/plans/2026-10-06-v3.7-themeA-implementation.md`）
- **主题B 运维硬化**（设计见 `docs/plans/2026-10-06-v3.7-themeB-implementation.md`）

### 主题 A：死区/RBE + EURange 生效

让此前"只存不下发"的 `deadband` / `eur_min`/`eur_max` 元数据真正生效：`rtdbd` 作为写权威在写入时施加 EURange 量程位，并按死区 / EURange 边界做**按订阅者**的 Reporting-By-Exception 通知。默认路径行为零变更。

### Added
- **语义纯函数公共 API**（`src/core/irt_semantics.c`）：`indurtdb_value_to_double`（6 种数值类型归一化）、`indurtdb_eurange_limit`（计算量程位 LOW/HIGH/NONE）、`indurtdb_deadband_exceeded`（绝对/百分比死区，含 `eur_max==eur_min` 除零保护）。
- **质量感知写入口**：`indurtdb_write_quality_ts` / `indurtdb_h_write_quality_ts`，一次 seqlock 写入「值 + 调用方算好的 quality（含量程位）」。
- **rtdbd 通知门控（RBE）**：新增 `rtdbd/src/rbe.c`，每连接定长 RBE 状态（内嵌于 `rtdbd_conn_t`），仅在死区越过或 EURange 边界跨越时推送 `NOTIFY`；首值必发、退订/重订阅重置、连接断开清理。状态仅在 rtdbd 进程内存，**零 ABI 变更**。
- **测试**：`tests/unit/test_c_rbe.cpp`（4 套件，覆盖各类型归一化 / 除零保护 / 双向 / 边界）；`tests/integration/test_rtdbd_proto_v2.cpp` 新增 `EurangeLimitOnWrite`、`RbeSuppressesNotify`、`NotifyEurCrossing`、`NotifyDeadbandPercent`、`NotifyDefaultUnchanged`。

### Changed
- `rtdbd` 的 `do_write` 改为：读取点位 meta → 计算量程位 → 经质量感知写入口落值（量程位在**写权威**统一计算，跨进程直读者看到一致语义）。
- 版本号四处同步为 3.7.0（`VERSION` / CMake `project(VERSION)` / `indurtdb.h` 宏 / CHANGELOG）。

### Notes / 已知限制
- **默认 `indurtdb_write_*` 行为完全不变**：核心热路径仍强制 `quality = GOOD`、**零 meta 读**（守住 v3.4 T3「meta 不进热路径」不变式）；仅 rtdbd 走新入口。`flags=0` 时 RBE 恒通知，由 `NotifyDefaultUnchanged` 守护。
- 仅开启 `INDURTDB_META_FLAG_EUR`（无死区）时，rtdbd 推送**仅在跨量程边界触发**，区间内变化不推送（值仍落库、GET 轮询可见）——「按订阅者 RBE」预期行为，非缺陷。
- `indurtdb_value_to_double` 对 `string` 等非数值类型返回 `0.0`，调用方责任仅对数值类型使用（RBE 仅对数值类型生效）。

### 主题 B：运维硬化（健康 / 背压 / 优雅退出 / 配置 fail-fast）

把"单测全绿但线上偶发"的隐患堵上，分三项。

#### B1 健康 / 错误计数 + 进程自检
- **核心自检 API**：`indurtdb_h_self_check()` / `indurtdb_self_check()`，纯读校验共享内存段头
  `magic` / `version`，返回 `INDURTDB_HEALTH_OK` / `UNHEALTHY`（`DEGRADED` 预留给调用方分层）。
- **rtdbd 运行统计**：新增 `rtdbd/src/stats.{h,c}`，定长计数器单例（零堆）：
  `n_writes` / `n_notifies` / `decode_fail` / `write_rejected` / `write_error` /
  `notify_drop` / `notify_send_fail` / `uptime_ns` / `health`。
- **`RTDBD_OP_HEALTH`（opcode 16）**健康快照：**只读、免鉴权**（同 `GET_LOG`，负载仅计数与
  连接数，不含点位值）。**协议主版本仍为 2**（仅新增 opcode，v3.6 monitor 行为完全不变）。
- **SIGUSR1 dump**：写入 `--stats-file`（默认 `/tmp/rtdbd.stats`）。信号处理器**只置
  `volatile sig_atomic_t` 标志**，实际 dump 在主循环执行（信号安全）。
- **健康判定用 60s 滑动窗口**：`DEGRADED` 可自愈，避免一次瞬时误请求让进程余生永久告警。

#### B2 背压 + 优雅退出
- **每连接有界出站队列**（`outq[64]` 定长环形，内嵌于 `rtdbd_conn_t`，零堆）：
  写成功只入队，队列满则**丢最旧（保序）**并计 `notify_drop`；主循环仅在
  `outq_count > 0` 时监听 `POLLOUT`。**慢消费者不再拖垮服务端**（消除 head-of-line 阻塞）。
- **`outq_flush` 用 `MSG_DONTWAIT` + `outq_sent` 做部分发送**：`POLLOUT` 就绪只保证缓冲有
  *部分*空间，若沿用阻塞 `send_all`，嵌入式小缓冲下会卡死整个单线程服务端。
- **连接关闭 / flush 失败的待发队列计入 `notify_drop`**，消除不可观测的静默丢失。
- **优雅退出**：SIGTERM/SIGINT 后**限时（≤ `RTDBD_SHUTDOWN_DELAY_SEC`）排空所有出站队列**
  再关 fd / unlink socket / 退出（超时不阻塞强退）。该宏此前已定义未启用，本版本正式启用。

#### B3 配置校验 + fail-fast
- **配置校验 API**：`indurtdb_validate_point_meta()`（纯函数）、`indurtdb_validate_config()`、
  `indurtdb_cfg_error_reason()`、`indurtdb_meta_pct_without_range()`。
  规则：EUR 启用时须 `eur_min < eur_max`；`deadband >= 0`；百分比死区 ∈ [0,100]；
  `flags` 仅含已知位；点位 `type` / `access` / 名称合法。
- **YAML 点位配置新增可选语义字段** `eur_min` / `eur_max` / `deadband` / `flags`
  （未声明则保持段内原值，向后兼容）。此前配置无法表达语义，该校验在生产上无从触发。
- **`indurtdb_h_set_meta()` 增加写入前语义校验**：非法 meta 一律拒写（`INDURTDB_ERR_ARG`），
  从源头堵住"非法 meta 落盘 → 下次启动 fail-fast"。
- **rtdbd 启动三阶段 fail-fast**（服务前完成，不进入半初始化态）
  | 退出码 | 含义 | 场景 |
  |---|---|---|
  | 0 | 正常退出 | — |
  | 1 | 通用 / 启动错误 | 参数解析、policy 加载失败等 |
  | 2 | 配置校验失败 | `load_config` 失败、点位语义非法 |
  | 3 | 共享内存损坏 / 初始化失败 | `indurtdb_initialize` 失败 |
  | 4 | 启动自检失败 | 段头 `magic` / `version` 不通过 |
- **supervisor 对致命码（2 / 4）停止 respawn** 并透传退出码，避免坏配置陷入重启风暴。

#### 主题 B Added / Changed / Notes
- **Added**：`indurtdb_detach()`——脱离实例但**保留共享内存段（不 `shm_unlink`）**。
  fail-fast 退出改用它：此前用 `indurtdb_shutdown()` 会因 owner 语义删除整段，
  把「配置写错」升级为「数据销毁」（评审 Critical 项）。
- **Changed**：`conns[]` 连接表移到文件域 `.bss`（B2 后每连接约 2KB，32 连接约 64KB，
  留在 `main` 栈帧对 SylixOS 小栈配置是隐患）。
- **Notes / 已知限制**：
  - 未给 `--config` 时，段内残留的非法 meta **只告警不致命**（避免 v3.6 时代部署被砖化）；
    给了 `--config` 才按严格模式 exit 2。
  - 百分比死区与 EUR 位**正交**：不要求二者同时启用。跨度无效（`eur_max <= eur_min`）时
    阈值退化为 0、死区永不触发，启动时打 WARN 而非拒绝——曾一度做成致命校验并误拒合法配置，已撤销。
  - **通知为异步投递**：B2 起写响应后通知在下一轮 `POLLOUT` 送达（缓冲有空间时亚毫秒级），
    不再与写响应同轮同步送达。
  - systemd 部署需配套 `RestartPreventExitStatus=2 4`：in-process 的「不 respawn」只挡得住
    rtdbd 自身，若 systemd 配了 `Restart=always`，重启风暴会转移到 systemd 层面。

### 主题 B 延展：B4 配置持久化 + B5 v1 API 退役（2026-10-07）

#### B4 运行时配置持久化（delta 日志）
解决「v3.6 CRUD 重启即丢」：运行时 `CREATE/DELETE/RENAME` 的点位跨重启保留。
- 新增 `src/core/irt_delta.{h,c}`：**定长 80B 记录**（静态断言锁死）、追加写 + `fsync`、
  回放解析（末尾残片丢弃；坏 `magic`/`version`/`op` 停止，已应用部分保留）。
- 新增 `indurtdb_enable_delta()` / `indurtdb_delta_replay()` 及 `_h_` 版本。
- `rtdbd --delta-file <path>`：**默认关闭**（未指定则行为与现状完全一致）；
  启用时启动顺序 `load_config(base)` → 回放 delta → 校验 → 服务。
- **base 配置不可变**：只追加 delta，不回写 YAML（保审计性、避免并发写冲突）。**ABI 不变**。

#### B5 v1 API 退役（技术债，**不含删除**）
- 新增 `INDURTDB_DEPRECATED` 宏，为 **51 个 v1 单例封装函数**标注弃用（编译期告警）。
- **`examples/` 与 `demo/` 已迁移到 v2 句柄 API**。
- 测试 / rtdbd / 诊断 harness / bench 脚本**刻意保留 v1**，经 `INDURTDB_NO_DEPRECATE_WARN` 关告警。
- **真正删除推迟到 v4.0**；v1 在此之前仍受支持且行为不变。

**为何不把测试整体迁到 v2（对 roadmap §3.4 的偏离，已评审确认）**：
roadmap 原文为「迁移 `examples/`、`demo/`、测试到 v2」，本版本只迁 examples/demo——
v1 到 v4.0 前都是受支持公共 API，测试正是它的回归网，整体迁移反而**减少**覆盖；
且约 500 处 / 35 文件的机械改动零行为收益却担全量 churn 风险；
消除告警用一行宏即可达成，成本收益不对称。

#### 延展项测试
- `tests/unit/test_c_delta.cpp`（8）：布局、空/缺失文件、顺序回放、残片丢弃、
  坏 magic/version/op 停止、append-replay 往返。
- `tests/integration/test_rtdbd_delta.cpp`（4）：CREATE 跨重启存活、DELETE/RENAME 持久化、
  未启用 delta 时行为不变、残片可容忍。

### 主题 B 测试
- `tests/unit/test_c_config.cpp`：新增 14 个 `ConfigValidate.*` 用例（并入原 3 个 `CConfig.*`，共 17）。
- `tests/integration/test_rtdbd_health.cpp`（5）：HEALTH OK、SIGUSR1 dump、`n_notifies` 语义钉定、
  deny→DEGRADED、坏负载→`decode_fail`。
- `tests/integration/test_rtdbd_config.cpp`（10）：合法启动、配置缺失→2、EUR 颠倒→2、
  未知 flags→2、百分比无跨度仍启动、instance 非法→1、max-points 0→1、max-subs 超限→1、
  **SET_META 非法被拒**、**exit 2 后段仍存在**。
- `tests/integration/test_rtdbd_backpressure.cpp`（3）：慢消费者丢最旧、快消费者不受影响、优雅退出码 0。

### 主题 B 评审修复（合并前）
- Critical：exit 2/4 曾 `shm_unlink` 销毁数据段 → 改用 `indurtdb_detach()` 保留段；
  同时补 `set_meta` 写入前校验 + 残留段降级为告警。
- `outq_flush` 阻塞风险 → `MSG_DONTWAIT` 部分发送。
- 连接关闭的静默通知丢失 → 计入 `notify_drop`。
- `DEGRADED` 永久锁存 → 60s 滑动窗口。
- 连接表占栈 → 移入 `.bss`。
- supervisor 留孤儿 worker → 新增 `terminate_and_reap()`（SIGTERM→退避等待→SIGKILL→回收）。

#### 复审（第二轮，针对上述修复本身）
- Critical：**部分发送帧混合**。队列满时若淘汰"已部分上线"的队首（`outq_sent > 0`），
  下轮会用新帧从旧偏移续发，线上出现「旧帧前缀 + 新帧后缀」的混合帧（长度仍 44B，
  只数帧数的测试会漏）。修复：队首已部分发送时**禁止淘汰**，改丢弃本次新通知。
- Critical：**supervisor 仍可能留活 worker**。`recv_all()` 的 EINTR 分支是
  `continue` 重试 recv、不检查 `g_stop`，故阻塞在 recv 的 worker 无法被 SIGTERM 终止。
  修复：`terminate_and_reap()` 用 SIGKILL 兜底；且内层 waitpid 重试循环在
  EINTR 且 `g_stop` 时改为主动终止回收（否则 supervisor 自己卡死在 waitpid，
  实测使 `test_rtdbd_recovery` 从 67ms 涨到 15.2s 并每测试留 1 个孤儿）。
- Important：优雅退出超时路径的剩余队列计入 `notify_drop`；`conn_discard_outq`
  触发健康窗口；config 数值严格解析（拒 NaN / 畸形 / 溢出，经 `bad_value` 判为配置非法）；
  `validate_point_meta` 加 `isfinite` 纵深防御；CLI `parse_u32_cli`（挡 `-1` → UINT32_MAX）
  与 `--max-points` 上界 1e6；supervisor fatal 路径清理 socket；supervisor 注册 SIGUSR1
  （避免文档中的 `kill -USR1` 打到 supervisor 时默认终止整个服务）。
- **API 修正**：`indurtdb_validate_config` 改为「返回状态码 + 输出 `bad_id`」——
  原契约用返回值 `0` 表示全部合法，但点 id 0 是合法点，非法点若为 id 0 会被误判为
  配置合法而绕过 fail-fast。

#### 测试护栏（复审新增）
- `test_rtdbd_recovery.NoOrphanWorkerAfterStop`：supervisor 停止后断言 worker 一并消失
  （**已验证回退修复后该用例挂死**，确为有效护栏）。
- `test_rtdbd_backpressure.FramesStayIntactUnderBackpressure`：重背压下逐帧校验
  magic / version / opcode / payload_len 与取值单调性。
  **诚实边界**：已实测该用例**不能**确定性复现帧混合（回退修复后仍通过）——
  该缺陷需「发送缓冲剩余空间 >0 且 <44B」的窄窗口，而 `MSG_DONTWAIT` 下缓冲满时
  `send` 直接返回 `EAGAIN`（0 字节），`outq_sent` 恒为 0。故此处仅作"帧结构完整性"
  回归守卫，不宣称覆盖该窄窗口；修复本身由代码评审确认（构造上禁止淘汰已部分上线的队首）。

---

## [3.6.0] — 2026-10-06「点位管控写 / 命令终端 / 运行日志」(设计见 `docs/plans/2026-10-05-rtdb-monitor-design.md` Phase 2)

把 `rtdb-monitor` 从「只读监控」推进到「可管理 + 类 Redis CLI + 运行日志」：点位支持运行时增/删/改名，提供命令终端、专业前端工程化界面与运行日志查看。

### Added
- **rtdbd 协议扩展（加法式，协议主版本仍为 2）**：新增三个**管控写** opcode（与 `OP_WRITE`/`OP_SET_META` 同权，走 `SO_PEERCRED` + uid 策略，deny by default）
  - `RTDBD_OP_CREATE_POINT`(12)：`id + type + access + name[64]` 注册点位（72B）。
  - `RTDBD_OP_DELETE_POINT`(13)：注销点位（清空 `name[0]` 并移除 name→id 索引）。
  - `RTDBD_OP_RENAME_POINT`(14)：`id + name[64]` 改名并同步索引（持写锁内原子完成）。
  - `RTDBD_OP_GET_LOG`(15)：拉取 rtdbd 运行日志（定长环形缓冲 128 条），`LOG_REQ` 4B / `LOG_ENTRY` 128B（`ts_ns` + `level` + `msg[116]`）。**只读、未走 uid 鉴权**，与 `OP_AUDIT_DUMP` 同策略（日志无 point 维度，且只含进程运行事件、不含点位值）；Web 层 `/api/logs` 仍受 `--token` 网关保护。
- **indurtdb 库新增 CRUD API**：`indurtdb_h_create_point/delete_point/rename_point` 及 v1 同名薄封装；复用 `load_config` 的 seqlock 写锁模式，索引用 `_locked` 变体保证原子性；建点位时拒绝同名（指其他 id）与已占用 id，删除/改名对不存在的点位返回 `NOT_FOUND`。
- **布局静态断言**：`CREATE_REQ` 72B / `DELETE_REQ` 4B / `RENAME_REQ` 68B 与 Python 客户端逐字节对齐。
- **rtdbd 运行日志**：新增 `logbuf.c/h` 定长环形缓冲（128 条、无堆分配、墙上时钟 CLOCK_REALTIME）；启动/监听/策略与配置加载失败、worker 重启、以及建/删/改名点位均写入日志，并保留既有控制台输出。
- **Python 客户端**：`create_point`（支持类型名如 `int32`）/ `delete_point` / `rename_point`；`get_logs(max_n)`。
- **Web 后端**：REST `POST /api/points`（新建）、`DELETE /api/points/{id}`、`POST /api/points/{id}/rename`（改名）、`GET /api/logs`；`POST /api/cmd` 命令终端支持 `ping/list/find/get/set/meta/create/del/rename/logs`（参数可用点位名）。
- **前端工程化**：迁移到 **Vite + Vue3 SFC + Element Plus**（构建产物 `web/dist` 由 FastAPI 托管）；点位表支持分页、名称/类型/**权限**（1=只读/3=读写）过滤、行内重命名与删除、新建点位对话框；新增**命令终端**页（历史/上下键/清屏）与**运行日志**页（时间/级别/消息、3s 自动刷新）；点位详情抽屉含实时曲线；深色模式与专业布局。
- **测试**：`tests/integration/test_rtdbd_proto_v2.cpp` 新增 `PointCrud`（create→get→rename→find→delete）、`PointCrudRequiresAuth`（默认策略须拒绝）、`GetLog`（首条为启动事件、create 后日志增长且含 `create point`）；`tests/test_rtdb_client.py` 新增 CRUD/日志编解码与类型解析用例；`tests/test_e2e.py` 覆盖 REST CRUD/日志与终端命令。

### Changed
- 前端由原生 HTML/JS（`static/`）迁至 `web/` 工程；`app.py` 优先托管 `web/dist`，未构建时回退 `static/`。
- 版本号四处同步为 3.6.0（`VERSION` / CMake `project(VERSION)` / `indurtdb.h` 宏 / CHANGELOG）。
- 合并前评审修复：`app.py` 的写类端点（`set`/`create`/`rename`/`cmd`）由 `async` 改为同步 `def`（避免阻塞事件循环，rtdbd 无响应时不再冻结 WS 推送）；启动连接 rtdbd 失败仅告警、Web 照常启动；`indurtdb_h_create_point` 索引插入失败（含 BUSY）一律回滚注册、约束 `access` 仅 1/3；前端权限显示/筛选对齐库常量（1=只读/3=读写），详情抽屉元数据改用真实字段（范围/死区/标志）。

### Notes / 已知限制
- 运行时 CRUD 落在**共享内存注册表**，进程重启后不保留（需持久化则另立版本规划，配合配置回写）。
- 新建点位受 `--max-points` 上限约束；点位类型受 8B `value_bits` 限制，字符串点位写值仍不可用。
- **点位分区（类似 Redis db0/db1）不在本版本**：已在 `docs/01-白皮书/01-产品白皮书.md` 演进路线登记为 **v3.7 规划项**，属数据模型层改动，待独立评估。

---

## [3.5.0] — 2026-10-05「RTDB 监控台 rtddb-monitor」(设计见 `docs/plans/2026-10-05-rtdb-monitor-design.md`)

新增可选监控工具 `tools/rtdb-monitor/`（纯 Python，零 C 依赖），提供类 Redis Insight 的 Web 监控台——枚举全部点位、实时值推送、按名/id 查看、设置点位值。

### Added
- **rtdbd 协议扩展（加法式，协议版本仍为 2）**：新增**只读、免鉴权** opcode
  - `RTDBD_OP_GET`(10)：按 `point_id` 读当前值（共享内存无锁读，不进入写热路径）。
  - `RTDBD_OP_LIST`(11)：枚举全部已注册点位（id/name/type/access），按 `max/offset` 分片流式返回。
  - 与 `FIND_BY_NAME`/`GET_META` 同权；写值仍走 `OP_WRITE`、元数据写走 `OP_SET_META`，继续受 `SO_PEERCRED` + uid 策略管控。
- **协议布局静态断言**：`rtdbd.c` 为 `GET_RESP`/`POINT_INFO` 等新增负载补 `RTDBD_STATIC_ASSERT` 锁死字节布局，与 `tools/rtdb-monitor/rtdb_client.py` 逐字节对齐。
- **Python 客户端 `rtdb_client.py`**：纯 `socket`+`struct` 复刻 `rtdbd/protocol.h` 报文，支持 PING/GET/LIST/WRITE/FIND_BY_NAME/GET_META 与 `SUBSCRIBE→NOTIFY` 后台推送线程。
- **Web 后端 `app.py`（FastAPI）**：REST（`/api/points`、`/api/points/{id}`、`/api/points/{id}/set`、`/api/meta/{id}`）+ WebSocket（`/ws` 实时推送 `update`，报文 schema 对齐 node-server `HmiPointValueDto`）；默认仅绑 `127.0.0.1`，可选 Bearer 校验。
- **原生前端 `static/`**：单页（无构建、`StaticFiles` 托管），点位表格 + 实时刷新 + 按 id 设值。
- **测试**：`tests/integration/test_rtdbd_proto_v2.cpp` 新增 `GetPointValue`/`ListPoints` 用例；`tools/rtdb-monitor/tests/test_rtdb_client.py` 协议编解码单测；`tools/rtdb-monitor/run_smoke.sh` + `scripts/run_monitor_smoke.sh` 端到端冒烟。

### Notes
- 写值权限最终由 rtdbd uid 策略决定；监控 Web 服务运行 uid 须在策略允许列表内（deny by default，无热加载，需重启 rtdbd 生效）。
- 新增 opcode 不 bump 协议主版本；若未来改负载布局需 bump `RTDBD_PROTO_VERSION` 并做协商拒绝。

## [3.4.0] — 2026-10-04「布局 v2 与 API v2」(路线乙，方案见 `docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md`)

**⚠️ ABI / 布局变更**：v3.4 段格式版本由 1 升至 2，Header 由 64B 扩至 128B。
**v1 段一律拒绝挂载**（返回 `IRT_SHM_ERR_VERSION`），需经迁移工具或停机清理后重建；**不支持新旧进程混跑**。详见方案 §6。

### Added（T1 — Header v2）
- 共享内存 Header 扩至 **128 字节**：新增 `crc32`(44)、`flags`(48)、保留位(52)、四个区段偏移 `off_points/off_index/off_meta/off_subs`(56–68)、`index_count`(72)、`scan_skipped`(76) 与保留区。前 44 字节（magic / version / max_points / max_subscribers / write_seq / owner_pid / stats）**偏移与语义不变**，便于迁移工具定位。`scan_skipped`  deliberately 置于保留区(76)、**不在 CRC 范围[48..71]**——否则运行时自增会使 attach 校验失败（见 T7）。
- **Header CRC32**（位运算实现，无查表、无静态存储）：覆盖**布局描述字段**（magic/version/容量 + flags/区段偏移），**刻意排除**易变字段（write_seq / owner_pid / stats / scan_skipped）——否则段一旦被写过 CRC 即失效，attach 将永远失败（T1 绿阶段实测踩到）。
- **版本协商**：段版本与布局不匹配即拒绝挂载并返回独立错误码 `IRT_SHM_ERR_VERSION`（-2），另区分 `IRT_SHM_ERR_ARG`(-1) 与 `IRT_SHM_ERR_CRC`(-3)。绝不按新布局解释旧段。
- **区段偏移写进 Header**：`irt_shm_points()` / `irt_shm_subscribers()` 改读 `hdr->off_*`，不再按 `sizeof(irt_header_t)` 硬算；布局计算集中于 `irt_layout_*()` 内联函数，为 T2（索引区）/ T3（元数据区）预留。

### Added（T2 — name→id 索引）

- **共享内存内 name→id 索引**（`src/core/irt_index.{h,c}`）：开放寻址（线性探测）+ 墓碑删除，条目 8B `{hash32, point_id}`，桶数 `roundup_pow2(max_points × 2)`（负载因子 ≤ 0.5），哈希 FNV-1a 32；冲突比较复用点位区内 `name[64]`，**不重复存字符串**。定长预分配、**零堆分配**。
- **公共 API `indurtdb_find_by_name(const char* name, uint32_t* out_id)`**：索引在共享内存内，对同段所有进程**全局一致**，集成方不必各自维护映射层。
- **语义化错误码**：`INDURTDB_OK / ERR_ARG(-1) / ERR_NOT_FOUND(-2) / ERR_FULL(-3) / ERR_NOT_INIT(-4) / ERR_BUSY(-5)`。既有接口仍返回 -1，只判 `< 0` 的旧代码行为不变。
- `indurtdb_load_config()` 在写入点位名后自动注册进索引（注册期集中写，不入热路径）。

### Added（T3 — 元数据区）

- **共享内存内元数据区**（`irt_shm_meta` / `irt_meta_set` / `irt_meta_get`，定义于 `irt_shm.{h,c}`）：每点 **32B** 冷数据，按 `point_id` O(1) 索引，布局与方案 §3.3 一致：`eur_min(8B) / eur_max(8B) / deadband(4B) / flags(4B) / reserved(8B)`。
- **公共 API** `indurtdb_get_meta(uint32_t id, indurtdb_meta_t*)` / `indurtdb_set_meta(uint32_t id, const indurtdb_meta_t*)`。返回值沿用 T2 语义化错误码（`OK / ERR_ARG(越界 id) / ERR_NOT_INIT / ERR_BUSY`）。
- **热路径隔离**：`irt_meta_set` 走全局 seqlock 写锁（注册/配置期），`irt_meta_get` 走无锁读重试；元数据区**不进入读写热路径**（用例 `Meta.NotInHotPath` 以字节级探针验证：1000 次点位值写读后元数据区原封不动）。
- 新增 `indurtdb_meta_t`（32B，`align(32)`），含 `IRT_STATIC_ASSERT(sizeof == 32)` 与字段偏移断言锁死 ABI。
- owner 建段时元数据区清零（`eur_min/max=0, deadband=0, flags=0`），符合 `Meta.DefaultIsZero`。
- 段布局新增元数据区：点位区 → 索引区 → **元数据区** → 订阅者心跳区；`off_meta / off_subs` 相应顺延。`test_c_shm` 总大小公式随之加入 `irt_layout_meta_size(max_points)`。

**段内存开销（累计）**：索引 `8B×roundup(2N)` + 元数据 `32B×N`。10000 点约 +256KB（索引）+ **320KB（元数据）** → 段约 **1.86MB**（v3.3 为 1.28MB）。均为一次性定长预分配，运行期不增长。

### Added（T4 — API v2 句柄化 / 去单例）

- **API v2 句柄化**：同一进程可同时持有多个实例。`indurtdb_t` 为不透明句柄，`indurtdb_h_open(indurtdb_t** out, instance_id, cfg*)` / `indurtdb_h_close(h)`；`indurtdb_cfg_t { max_points, max_subscribers }`。
- **全量 v2 函数**：`indurtdb_h_*` 覆盖 v1 全部操作（读写 / 区间 / 订阅 / 配置 / 心跳 / 索引 / 元数据 / 校验 / 统计），入参首位是 `indurtdb_t* h`，返回语义化错误码（`OK / ERR_ARG / ERR_NOT_FOUND / ERR_FULL / ERR_NOT_INIT / ERR_BUSY`）。
- **v1 保留为薄封装**：v1 全局函数 = 默认句柄 `g_default` 的转发，行为**完全不变**（既有调用方零改动）。`g_default` 仍走原 fork 检测逻辑（子进程自动重置 owner）。
- **实例表定长零堆**：`static indurtdb_t g_registry[64]` + 位图 `g_used_mask`，满足"核心层无堆"不变式；槽位耗尽管 `indurtdb_h_open` 返回 `ERR_FULL`。
- 公共头引入 `INDURTDB_DEPRECATED` 预留宏（本期**未**标注 v1，因 -Werror 会使调用 v1 的既存测试/示例编译失败）；v1 标注 `deprecated` 并移除推迟到 v3.5（届时同步迁移测试与调用方）。

**实测 / 验证**：
- 用例 `ApiV2.MultipleInstancesInOneProcess`：同进程两实例写同一 id 互不污染。
- `ApiV2.FindByNameIsPerInstance`：同名 `sensor.x` 在两实例注册到不同 id，各自只解析到自己（索引按实例隔离）。
- `ApiV2.SingletonRegression`：旧 `indurtdb_initialize` 仍可用。
- 22 个既有 v1 测试全量无回归。

### Changed
- 段布局 v2 新增元数据区段（v3.4 T3）。
- Header 新增 `index_count`（offset 72，取自保留区）：索引当前装载条目数，供巡检/诊断使用（易变，不入 CRC）。

**实测（负载 0.5，桶 1024 / 装载 512）**：平均探测长度 **0.246**，最长 **7**，冲突率 **14.1%** —— 远优于 O(B) 最坏界。满负载（对抗性填满全部桶）查找仍在**有限步内终止**并报 `ERR_FULL`，不死循环、不越界写。

### Changed
- 段格式版本 `IRT_SHM_VERSION` 1 → 2。
- 段布局：点位区与订阅者心跳区之间插入**索引区**（预留的元数据区在 T3 启用）。既有布局测试随之改为按 `hdr->off_*` 断言，不再假设"点位区紧邻心跳区"。
- 既有布局测试随之上移：`test_c_layout_seqlock` 期望 Header 128B / version 2；`test_c_shm` 总大小公式改为 `128 + N*128 + 索引区 + M*16`。

### Notes
- 点位 `sizeof(indurtdb_point_t)` 仍为 **128**、既有字段偏移不变；bench 回归无退化（P99 peek / read / write 均 PASS）。
- **段内存开销**：新增 Header 64B + 索引区 `8B × roundup_pow2(2N)`。例：10000 点由 1.28MB → 约 1.54MB（索引 256KB）。索引为一次性的定长预分配，运行期不增长。
- 升级流程：停机 → 迁移（T5 工具）或清理 `/dev/shm/indurtdb_*` → 全进程同时升级。回退：重新部署 v3.3.x + 清理段。

### Added（T5 — 离线迁移工具 + 双版本共存验证）

- **离线迁移工具 `tools/irt_migrate`**（纯 Python，符合工具集约定，不依赖 C 库）：`v1` 段 → `v2` 段。
  - 逐字节搬运点位值 / 类型 / 质量 / `timestamp_ns`（入库时刻）；点位名随点位搬运，并在 v2 段内**重建 name→id 索引**（FNV-1a + 开放寻址 + 墓碑，与 `src/core/irt_index.c` 算法逐字节一致）。
  - `source_timestamp_ns`（采集时刻）默认**置 0**（`--source-ts-mode keep` 可保留）；元数据区清零、订阅者心跳区清零（离线迁移，由新 `rtdbd` 重建）。
  - Header 按 v2 布局构造并写入 **CRC32**（覆盖 `[0:16]+[48:72]`，与 `irt_header_crc32_of` 逐字节一致，复用标准 CRC-32/IEEE 802.3）。
  - **版本保护**：源段非 v1（即已是 v2）时拒绝迁移，避免误覆盖。
  - 子命令：`migrate --src <id> --dst <id> [--source-ts-mode zero|keep] [--force]`、`validate`、`info`；迁移后自动执行独立校验。
- **独立校验**（与实现语言解耦）：重新解析 v1 源段与 v2 目标段，逐项比对容量 / 点位字节 / 索引一致性 / Header CRC，任一不一致即报错。
- **双版本共存验证**：`v3.4` 库对 v1 段**版本协商拒绝挂载**（`IRT_SHM_ERR_VERSION`），印证"不支持新旧进程混跑"；迁移工具对 v2 源段拒绝，印证离线工具只认 v1。
- **集成测试 `tests/unit/test_c_migration.cpp`**（TDD 红→绿）：进程内造 v1 段 → 驱动 Python 工具迁移 → 用**真实库 `indurtdb_h_open` 回挂 v2 段**并逐项校验（点位字段 / `source_ts` 清零 / `find_by_name` 索引）；库能成功 attach 即证明 v2 段字节级兼容。并验证 v3.4 拒绝挂载 v1 段。
- **工具自测** `tools/irt_migrate/tests/test_migrate.py`：布局尺寸 / CRC / FNV-1a / 索引 / 完整迁移往返 / 版本拒绝，可直接 `python3 tests/test_migrate.py` 运行（无需 pytest）。

### Added（T6 — 语义补齐：类型扩展 + 质量 OPC UA 映射）

- **类型扩展**：点位 `value` union 新增 `int64 / uint32 / float`，复用既有 32B 联合体（**布局不变**，结构体仍为 128B、`align(128)`）。新增枚举 `INDURTDB_TYPE_INT64(4) / UINT32(5) / FLOAT(6)`。
- **读写 API（v1 + v2 句柄）**：`indurtdb_{h_}write_int64/uint32/float`、`read_int64/uint32/float` 及携带采集时刻的 `write_*_ts`（v1 薄封装指向默认句柄，行为不变）。`irt_point_manager` 的 `pm_write_impl` switch 与 `_ts` 包装补齐三类型；读路径沿用既有模式（直接读 union 成员，与现有 int32/double 一致）。
- **质量 OPC UA 映射（纯函数，无状态）**：`indurtdb_quality_to_status_code(uint8_t)` / `indurtdb_status_code_to_quality(uint32_t)` 双向可逆。base 码决定 severity(bit30-31)+子状态(code,bit0-15)，**量程位映射到 StatusCode 保留位(bit28-29)**（远离 severity、且避开 OPC UA 已定义位 bit24/25，仅用于本库产物回读，不得原样投递第三方 OPC UA 栈），与 severity/code 正交；未知 StatusCode 回退 `BAD`。映射表参考 OPC UA Part 4（Good/Bad/Uncertain、LocalOverride、OutOfService、NoCommunication、SensorFailure、ConfigurationError、LastUsableValue、InitialValue、SensorNotAccurate），可按现场需求调整。
- **可用性判定**：`indurtdb_quality_is_usable(uint8_t)`——「值是否可用」只看基础码（量程位正交）。
- **quality 分层宏**（bit0–3 基础码 + bit4–5 量程位 + bit6–7 预留，`QUALITY_BASE/LIMIT/MAKE` 与 0–10 基础码）已随 v3.4 Header 落地并由 `test_c_point_fields` 覆盖，本任务补齐其北向映射与可用性语义。
- **质量写契约**：`write_*` 将 quality 置为纯 `GOOD`（量程位清零）；如需量程位，应在写之后调用 `set_quality`。`check_timeouts` 超时刻**保留**量程位（基础码改为 `TIMEOUT`，量程位不丢）。
- **测试** `tests/unit/test_c_semantics.cpp`（11 例）：类型扩展 v1/v2 往返（int64 负值 / uint32 大值 / float / `_ts` 保采集时刻）、StatusCode 全 base×limit 双向往返、已知常量、未知回退 BAD、量程位存活、is_usable 只看基础码。

### Added（T7 — 可靠性：issue #19 修复，热写者下超时扫描不再饿死）

- **L1 写锁冲突让步退避**：`irt_seqlock_write_begin` 遇他人持锁（seq 奇数）不再立即返回，改为**有限次重试（默认 3 次）+ 指数退避 + `sched_yield()`**，给持锁方完成窗口。修复 issue #19——热写者无间隙自旋时，全局单 seqlock 偶数窗口极短，`check_timeouts()` 整轮取不到写锁而返回 0。写路径无冲突时零开销（首抢即中，不进入退避）。
- **L2 可观测计数**：Header 新增 `scan_skipped`（offset 76，保留区，**刻意置于 CRC 范围[48..71]之外**——否则运行时自增会使 attach 校验 CRC 失败而拒掉合法段）。超时扫描在让步退避后仍冲突、跳过某点时 `__atomic_fetch_add(&hdr->scan_skipped, 1)`，使"饿死"可被运维观测而非静默。
- **新 API**：`indurtdb_get_scan_skipped()` / `indurtdb_h_get_scan_skipped(h)`（v2），返回被跳过的点数累计。
- **测试** `tests/unit/test_c_reliability.cpp`（3 例）：`NotStarvedByHotWriter`（热写者自旋下全部陈旧点仍被检测，修复前 ≈0）、`SkippedIsObservable`（skip 计数 > 0）、`NoRegressionOnIdle`（无并发写时全检测且 scan_skipped 恒 0）。
- **注意**：`scan_skipped` 布局位从初版的 52 调整为 76（保留区），因 52 落在 CRC 覆盖区[48..71]内，运行时自增会破坏 CRC；既有字段偏移与迁移工具 CRC 字节级兼容保持不变。

### Added（T8 — 诊断与工具集：巡检 / 冒烟 / 泄漏检测 / 覆盖率）

- **诊断工具 `tools/irt_diag`**（纯 Python，`stdlib` only，不依赖 C 库；自带 `pyproject.toml` / `README.md` / 模块划分）：
  - `inspect`：在线读取 `/dev/shm/indurtdb_<id>` Header，输出版本 / 点数 / CRC / owner pid / 段大小，并判定 `healthy`。CRC 覆盖区间（`[0:16]+[48:72]`）与 `irt_header_crc32_of` 逐字节一致；`scan_skipped` 位于保留区[76:80]、不计入 CRC。
  - `smoke`：驱动 C harness 完成「起服务 → 写 → 读 → 订阅 → 停」全流程，退出码透传。
  - `leak`：驱动 harness 反复启停，比较 `/proc/self/fd` 计数，**检测 fd 增长**（delta>0 即告警）。
  - 统一参数 / 退出码 / 日志，可独立运行，可接入 CI。
- **C harness `tools/irt_diag/c_harness/smoke_leak.c`**（由 CMake 构建为 `irt_diag_harness`，仅 Linux）：复用公共 API，零 C++ 依赖；冒烟逐步骤打印 `SMOKE_*` 标记，泄漏模式打印 `LEAK_INIT_FD` / `LEAK_FINAL_FD`。
- **覆盖率插桩**（TDD 红→绿的基础设施）：顶层 `INDURTDB_ENABLE_COVERAGE=ON` 以 `--coverage -O0 -g` 构建库；`coverage` 自定义目标运行 `ctest` 并（若 `gcovr` 可用）生成 `coverage.xml` / `coverage.html`。
- **测试** `tests/unit/test_c_diag.cpp`（4 例）：`Tools.InspectReportsHealth`（巡检输出版本/点数/CRC/owner 且 crc_ok）、`Tools.SmokeEndToEnd`（harness 全流程退出 0）、`Tools.DetectsFdLeak`（反复启停无 fd 增长）、`Coverage.TargetBuildsAndRuns`（coverage 配置+构建库通过并产出 `.gcno`）。

### Added（T9 — rtdbd / irtcli 协议 v2）

- **协议版本 `RTDBD_PROTO_VERSION` 1 → 2**：新增 opcode `OP_FIND_BY_NAME`(7) / `OP_GET_META`(8) / `OP_SET_META`(9)，及状态 `RTDBD_ST_NOT_FOUND`(4)。线结构 `rtdbd_find_req_t`(name[64]) / `rtdbd_find_resp_t`(point_id) / `rtdbd_meta_payload_t`(32B，与 `indurtdb_meta_t` 逐字段布局一致，服务端零转换 `memcpy`) / `rtdbd_meta_req_t` / `rtdbd_set_meta_req_t`。版本不匹配仍**立即关闭连接**（不静默），新旧客户端不混跑。
- **服务端 `rtdbd`**：`FIND_BY_NAME` / `GET_META` 为只读、无需鉴权；`SET_META` 为**管控写**，复用 `SO_PEERCRED` 取对端 uid，经 `irt_policy_allows`（`deny by default`）鉴权，失败返回 `RTDBD_ST_DENIED`，成功记录审计。新增 `--config <path>` 在启动时调用 `indurtdb_load_config` 注册点位名进共享索引（rtdbd 作为索引注册方，使 FIND_BY_NAME 端到端可用）。
- **客户端 `irtcli`**（同步请求-响应，绕过异步写队列）：`irtcli_find_by_name` / `irtcli_get_meta` / `irtcli_set_meta`；返回码新增 `IRTCLI_ERR_NOT_FOUND`(-6)，服务端状态 `DENIED/NOT_FOUND` 映射为对应客户端码。通用 `rt_submit` 助手处理连接/收发/重连。
- **测试** `tests/integration/test_rtdbd_proto_v2.cpp`（4 例）：`ProtoV2.FindByName`（按名查到 id，未注册返回 NOT_FOUND）、`ProtoV2.MetaWriteRequiresAuth`（默认策略拒写元数据）、`ProtoV2.MetaWriteAuthorizedSucceeds`（授权 uid 写成功且读回 round-trip）、`ProtoV2.RejectsV1Client`（v1 客户端被断连）。

### Notes（v3.4.0）
- **ABI 冻结**：共享内存段版本固定为 `IRT_SHM_VERSION = 2`；Header v2（128B）、name→id 索引区、32B 元数据区布局与字段偏移均经静态断言锁死。v1 段被版本协商拒绝挂载，升级需经迁移工具或停机清理后重建。
- **SylixOS / ARM**：本机 x86_64 实测全绿；**SylixOS / ARM 未实测**，按 T10 风险约定**不阻塞**发布，待硬件到位后补实测数据。
- **v1 全局 API**：v3.4 仍全部保留且行为不变；计划于 **v3.5** 起标注 `deprecated` 并最终移除（届时同步迁移测试与调用方）。
- 发布 tag：`v3.4.0`。

---

## [3.3.0] — 2026-10-02「读写分离与双通道」

### Added
- `rtdbd` 写权威守护进程：UDS 监听、极简二进制协议（magic + 版本，不匹配即拒）、单线程串行写、`SO_PEERCRED` 鉴权、定长审计环形缓冲（T1）。
- `indurtdb-client`（`irtcli_*`）：本地写队列（默认 256，同点位合并去重，满则告警不静默丢）、断连重放、同步/异步两种写入模式（T3）。
- 跨进程变更通知：复用已建立的 UDS 连接广播变更，订阅者收到 `RTDBD_OP_NOTIFY`（T4）。
- `source_timestamp_ns`（偏移 112，占原 padding）与 `COMM_FAILURE` 质量码落地，**`sizeof(indurtdb_point_t)` 仍为 128，既有字段偏移不变，ABI 保持 v1**（T5）。
- supervisor 自动拉起 + 重启 attach 已有段（不重建），RTO 实测 ≈12ms（T2）。
- 端到端混合角色测试 `tests/integration/test_e2e_mixed.cpp`：驱动（客户端写）→ rtdbd → 共享内存 → 控制逻辑（核心库直读）/ HMI（跨进程通知），含服务重启后重放场景（T6）。
- `scripts/run_e2e.sh`：端到端脚本（清理残留守护进程/socket/shm → 构建 → 混合角色用例 → rtdbd 集成回归），已接入 CI 双配置。
- UDS 往返延迟实测报告 `docs/06-开发规划/11-v3.3-T0-UDS延迟实测报告.md`（16B：P50 11.585μs / P99 18.182μs；门槛通过）。

### Fixed
- **`indurtdb-client` 未屏蔽 `SIGPIPE`**：`rtdbd` 崩溃后客户端向失效 socket 写入会被内核投递 `SIGPIPE` 直接终止，与 fail-operational 目标相悖。改为 `send(..., MSG_NOSIGNAL)`，失败由返回码表达（T6 端到端发现并修复）。
- `irtcli_flush()` 在旧连接失效时不重连，导致服务恢复后需二次 flush 才重放。现 IO 失败后先重连再重试一次，"恢复后一次 flush 完成重放"成立。
- 测试守护进程继承测试进程 stdio：父进程异常退出后成为孤儿并持有输出管道，挂住 ctest / CI 采集。测试中改为 `setsid()` + stdout/stderr 重定向到 `/dev/null`。
- `test_c_quality` 的 `TimeoutDetectionConcurrentWriteSurvives` 使用 `timeout_ns = 1`，断言依赖"并发写恰好落在纳秒级窗口内"，属**竞态断言**：Debug 构建与 CI 多任务争用下 writer 线程被抢占即失败（CI Debug 红灯、Release 绿）。改为确定性超时窗口（1s），并新增 DQ-07「陈旧点位在并发写期间仍须被标记」补回覆盖率（写者限速 200µs 保证确定性）。
- 附带发现并**文档化**边界：`check_timeouts()` 在写入者无间隙自旋时可能整轮取不到全局 seqlock 而返回 0（真实采集周期下不触发）——已写入 README / SDK 手册约束第 4 条，并立 [issue #19](https://github.com/turnarond/InduRTDB/issues/19) 供 v3.4 评估。

### Changed
- **版本一致性由构建系统守护**：`test_c_version` 不再硬编码版本号，期望值由 CMake `project(VERSION)` 注入（`INDURTDB_EXPECT_VERSION_*`）。此前每次发布都需手改测试，否则红灯（本次发布实测踩到）。
- 白皮书 4.0 → 4.1、用户体验白皮书 4.0 → 4.1：新增读写分离与双通道、受控通道实测延迟、跨进程通知已可用，并修正"不支持跨进程通知""无鉴权审计"等已过时表述。
- SDK 手册新增「indurtdb-client（受控通道 API）」章节（`irtcli_*` 函数、返回码与语义）。

### 发布信息
- 版本号四处同步：`VERSION` / CMake `project(VERSION)` / `indurtdb.h` 版本宏 / README + CHANGELOG 均为 **3.3.0**。
- **ABI 保持 v1**：`sizeof(indurtdb_point_t)` 仍为 128，既有字段偏移不变，与 v2.x 逐字节兼容。
- 发布前按 `docs/06-开发规划/09-文档一致性检查清单.md` 逐项勾选（见该文"v3.3.0 勾选记录"）。
- 验证：Debug / Release 零警告；`ctest` 22/22；`verify_consume.sh`、`run_soak.sh`(30s)、`run_bench.sh --quick`、`run_e2e.sh` 全部通过。

---

## [3.2.0-docs] — 2026-09-23「文档治理基线」（仅文档，库版本仍为 3.1.0）

### 定位统一
- 对外口径统一为「**node-server 底层共享内存实时数据层**」；北向 OPC UA、持久化/历史、集群同步一律归 node-server / Bridge，本库不重复造轮子。

### Added
- **统一版白皮书 4.0**（`docs/01-白皮书/01-产品白皮书.md`）成为对外口径唯一来源。
- `docs/06-开发规划/08-功能评审与下一步规划.md`：功能盘点、竞品对标（iceoryx / IoTDB / openHistorian / OPC UA）、差距清单 G8–G20、版本边界。
- `docs/06-开发规划/09-文档一致性检查清单.md`：发布前 Release Checklist，防止文档腐败。
- README / 白皮书 / SDK 手册统一声明 **6 条已知行为约束**（peek 单拷贝、单进程单例、写冲突 busy 不重试、超时检测 best-effort、订阅为进程内回调、无鉴权/加密/持久化）。
- `docs/05-SDK手册/03-C-API参考手册.md` 补齐缺失的 `indurtdb_check_timeouts()` 条目。

### Changed
- 白皮书 1.0 / 2.0 移入 `docs/01-白皮书/归档/` 并加"口径废弃"横幅；用户体验白皮书升级至 4.0 并重编号为 `02-用户体验白皮书.md`。
- `docs/README.md` 更新目录索引、阅读路径与文档约定（含 Release Checklist 要求）。
- `.gitignore` 增加 AI / 插件中间目录忽略：`.codegraph/`、`.superpowers/`、`.omc/`、`.claude/`、`.codebuddy/memory/`。

### Fixed（11 条文档与实现不符）
1. `peek()` "零拷贝、返回共享内存指针" → 实为**单拷贝到 `_Thread_local`**
2. "多实例隔离 / 分片 ✅" → 实为**单进程单例**（跨进程实例隔离正常）
3. OSAL 抽象 `IThreading` / `INotification` → 实仅有**时间 + 共享内存**两类能力
4. "24 个函数" → 实为 **26 个函数**
5. "订阅推送延迟 ≤50μs（通知到订阅进程）" → **跨进程订阅不存在**，指标删除
6. 集群 / VSOA RPC 作为卖点 → **未实现且已不在边界内**，口径下线
7. 安全对比表把 VSOA 认证/加密算作本库能力 → 本库**零鉴权零加密**，`access` 是静态只读标记
8. 吞吐"≥50k 点/秒" → 替换为 **x86 实测值**（5.2M op/s）
9. 未声明多写者写冲突语义 → 明确 **`write_*` 返回 -2（busy），不阻塞、不重试**
10. 测试数量口径不清（各处写 14 / 15 / 26 不等） → 统一为**双口径**：gtest **26 套件 / 126 用例**，ctest 注册 **16 个**（15 单元 + 1 多进程集成）。两者是不同粒度，此前被误当成互相矛盾（SRS / 演进规划 / Seqlock 技术文档 / 用户体验白皮书 / 评审文档 / CODEBUDDY.md 均已统一）
11. `04-技术文档/01-Seqlock算法设计文档.md` 仍描述已移除的 `irt_seqlock_read()` → 已标注"早期设计、已移除"并说明现行单拷贝 `peek` 实现

### Notes
- 本次为**纯文档版本**：不改代码、不改 ABI，**库版本号保持 3.1.0**（`VERSION` / CMake / `indurtdb.h` 宏不变），避免"版本号前进但功能未变"造成误导。
- 建议打 tag `v3.2.0-docs` 作为文档基线；v3.3 起进入能力版本，届时再同步 bump 库版本号。

---

## [3.1.0] — 2026-08-09

### Added
- **崩溃恢复** (irt_shm): owner 进程崩溃后, attacher 通过 `kill(pid, 0)` 存活检查接管所有权 (`irt_shm_os_claim_ownership`)
- **Seqlock 奇数恢复**: attach 时检测到遗留的奇数 write_seq (写锁内崩溃) 自动推进到偶数, 恢复一致性
- header 填充区新增 `owner_pid` 字段 (128 字节布局保持不变)
- OSAL 新增 `irt_shm_os_claim_ownership()` 接口 (posix/sylixos 双平台)
- **集成产品化**: 三种方式将 InduRTDB 作为库引入 C/C++ 工程
  - `find_package(InduRTDB)`: install 规则 + CMake 包导出 (Config/Version/Targets)
  - pkg-config: `indurtdb.pc` 生成与安装
  - FetchContent / add_subdirectory: 源码内嵌, 默认子项目隔离
- `InduRTDB::indurtdb` 命名空间目标 (三条消费路径统一链接名)
- 子项目隔离: `INDURTDB_IS_TOP_LEVEL` 检测 + `INDURTDB_BUILD_TESTS`/`INDURTDB_BUILD_EXAMPLES` 选项门控
- 版本宏 `INDURTDB_VERSION_MAJOR/MINOR/PATCH/STRING` + 一致性测试 (`test_c_version`)
- 开箱即用 starter 模板 (`templates/indurtdb-starter`, find_package/FetchContent 双模式)
- 外部消费回归脚本 (`scripts/verify_consume.sh`, 含 pkg-config 前缀一致性校验)
- 集成指南 (`docs/05-SDK手册/04-集成指南.md`)

### 产品化交付 (v3.1.0 发布缺口, 2026-08-10)
- **测试覆盖强化**: 边界/异常/质量路径 49 用例 (`test_c_edge_cases`)
- **soak 长稳测试**: 多进程持续读写 + 数据完整性校验 (`tests/soak/`, `scripts/run_soak.sh`)
- **故障注入测试**: kill-owner 崩溃自愈 / 奇数 write_seq 恢复 / SIGKILL 活跃 owner (`tests/soak/fault_injection_test.c`)
- **x86 性能基准**: P99 write_int32 0.39μs / read_int32 0.07μs / peek 0.06μs, 全部超越 10μs 设计目标 (`tests/bench/`, `scripts/run_bench.sh`)
- **CI 流水线**: GitHub Actions, Debug/Release 双配置, build+ctest+verify_consume+soak+bench (`.github/workflows/ci.yml`)
- **定位与边界文档**: README 前列定位/边界表, 明确与 node-server 分工

### Changed
- demo 改为经 pkg-config 消费已安装库 (`demo/Makefile`, `INDU_PREFIX` 可覆盖)

### Fixed
- fork 后子进程 owner 标记未清除导致 `shm_unlink` 误删父进程共享内存段 (`indurtdb_initialize`)

### Notes
- VERSION 文件、CMake project(VERSION)、indurtdb.h 版本宏已统一为 3.1.0
- `indurtdb.pc` 的 `prefix` 在 configure 阶段写入; 安装前缀须经 `-DCMAKE_INSTALL_PREFIX` 指定

---

## [3.0.0] — 2026-07-27

### 概述
纯 C 重写。所有模块从 C++ (v2.x) 直译为 C11，保证共享内存布局逐字节一致。

### 收益
- **API 面积极小**：全部功能浓缩为单一头文件 `indurtdb.h`，仅 100 行、26 个函数，学习成本几乎为零
- **零 C++ 运行时依赖**：不依赖 STL、异常、RTTI、虚表，可在任何 C11 编译器上构建和链接
- **代码量大幅缩减**：核心 C 源码仅 ~760 行 + 内部头 ~300 行，远少于原 C++ 实现（含 C API 桥接层）
- **编译速度显著提升**：无模板展开、无 STL 头文件包含链，增量编译接近 C 编译速度
- **确定性执行**：去除虚函数调用（间接跳转），所有调用路径编译期确定，更利于 WCET 分析
- **跨语言 FFI 原生兼容**：纯 C API 可被 C++、Python ctypes、Rust FFI、Go cgo 直接调用，无需桥接层
- **SylixOS 友好**：避免 SylixOS 对 C++17 特性支持不完整的问题（特别是 stdatomic），全部使用 `__atomic` builtins
- **单例无锁设计**：全局 Seqlock + 无堆分配，运行时无内存碎片，适合 7x24 工业场景

### Added
- 纯 C 公共 API (`include/indurtdb/indurtdb.h`): 26 个函数，单头文件，零 C++ 依赖
- C11 OSAL 层 (`irt_osal.h`): POSIX + SylixOS 双平台，无虚表
- irt_shm 共享内存段管理: shm_open/mmap, owner 检测, magic/version 校验
- irt_point_manager: 4 种类型写入 (bool/int32/double/string), seqlock 读, 单拷贝 peek, 超时检测
- irt_subscription: 回调注册/通知/心跳/僵尸清理, 定长 Slot 数组
- irt_config: 轻量 key=value 解析器 + YAML 点位元数据解析
- 单元测试 x14 (C API, config, layout+seqlock, osal, point_manager, shm, subscription, boundary, concurrency, data_model, multi_instance, performance, quality)
- 集成测试 x1 (多进程 fork + 布局回归): 6 用例, 覆盖父子进程读写和原始字节布局校验
- `indurtdb_peek()`: 单拷贝读取, 返回线程本地缓冲指针（同线程下次 peek 覆盖, 需长期持有请用 read_point）
- `indurtdb_read_range()` / `indurtdb_write_range_*()`: 批量读写接口
- `indurtdb_subscribe()` / `indurtdb_unsubscribe()`: 变更订阅
- `indurtdb_load_config()`: 从 YAML 配置文件加载点位元数据
- `indurtdb_update_heartbeat()` / `indurtdb_is_initialized()`: 心跳与状态查询
- `indurtdb_check_timeouts()`: 超时检测, 将超时点标记为 QUALITY_TIMEOUT
- `indurtdb_get_write_count()` / `indurtdb_get_timeout_count()` / `indurtdb_get_last_error()`: 统计与错误查询
- _Thread_local error storage (indurtdb_get_last_error)
- Shared memory layout byte-identical to v2.x
- C11 _Static_assert for layout verification

### Changed
- **语言**: C++17 → C11 (gcc), 测试保留 C++17+gtest
- **构建**: `-std=c++11` → `-std=c11` (库), `-std=c++17` (测试)
- **编译选项**: C 文件 `-Wall -Wextra -Werror`
- **头文件**: `<indurtdb/api/c/indurtdb_c.h>` → `<indurtdb/indurtdb.h>`
- **单例模式**: C++ static local + PIMPL → C static global struct
- **API 设计**: 模板 write<T>() → 显式类型函数 (write_bool/int32/double/string)
- **模块命名**: irt_ 前缀 (InduRTDB C 实现), 内部头不暴露给用户
- **测试框架**: 7 个独立 test suite → 统一 C API 测试套件
- **项目结构**: include/ 精简为单一公共头, src/ 按 core/api/osal/internal 组织

### Fixed
- 消除所有虚函数开销 (OSAL, PointManager, SubscriptionManager)
- 消除所有 STL 容器依赖 (vector/map/function/mutex/unique_ptr)
- 消除所有异常处理路径
- 修复 Seqlock 多进程 ABA 防护 (uint64_t 保证不溢出)

### Removed
- C++ PIMPL 实现层 (`src/api/cpp/`)
- C ABI 桥接层 (`src/api/c/`)
- `ISeqlock` / `ISharedMemory` / `ITime` / 所有虚接口
- `std::function` / `std::vector` / `std::unordered_map` / `std::mutex`
- POSIX C++ 封装 (`src/osal/posix/` C++ 文件)
- C++ 单元测试 (API/config/layout/seqlock/osal/pm/shm/sub — 全部以 C 重写)
- `SeqlockFactory` / `SubscriptionManagerUtils` 等工具类

### 兼容性
- 共享内存布局逐字节兼容 v2.x: Header (64B) + PointData (128B) + SubscriberEntry (16B)
- magic (0x1DBA1DBA), version (1) 保持不变
- C API 函数签名与 v2.x C ABI 向后兼容

---

## [2.1.0] — 2026-05-11

### Added
- 多进程集成测试 (6 用例)：AllDataTypes, ZeroCopyPeek, MultipleReaders, BulkMixedTypes, HeartbeatVisible, InstanceIsolation
- `safe_fork_and_run` 模式：子进程通过 `_exit()` 安全退出，避免继承的析构函数错误地 `shm_unlink`
- 轻量 YAML 配置解析器 (`ConfigLoader`)，零第三方依赖
- `InduRTDB::write(PointId, const char*)` 非模板重载，修复字符串字面量类型推导
- `SharedMemorySegment` 公共头文件 (`shared_memory_segment.hpp`)
- `VERSION` 文件

### Changed
- **Seqlock 重构**：200+ 行 OOP 层次（ISeqlock/SeqlockException/Factory/Utils）→ 70 行轻量 inline 自由函数 (`seqlock_write_begin/end/read`)
- **PointManager 重写**：虚接口 + `vector<unique_ptr<ISeqlock>>` → 非虚类 + 直接操作共享内存 `PointData*` 数组 + 全局 `header_->write_seq` Seqlock
- **SubscriptionManager 重构**：`std::unordered_map`/`std::vector`/`std::function`/`std::mutex` → 定长 `SubscriberSlot[256]` + C 函数指针回调
- **API 层**：去掉全局 `std::mutex`，`subscribe`/`loadConfig`/`updateHeartbeat` 从 stub 改为完整实现
- **C ABI**：17 个函数从 stub 改为完整桥接实现
- **OSAL**：删除重复的 `linux/` wrapper 层和废弃的 `interface/` stubs，统一为 POSIX 实现
- SharedMemorySegment 修复：`unique_ptr<ISharedMemory>` 生命周期从局部变量提升为成员变量
- PointManager::peek() 从 `static temp` 拷贝改为真正零拷贝（直接返回 `&points_[id]`）
- 构建：`libindurtdb.a` 编译通过，零警告；59 tests 全部通过

### Fixed
- Seqlock `s1` 可能未初始化 (`-Werror=maybe-uninitialized`)
- `SubscriptionManager` 测试缓冲区溢出（`shm_table_[4]` 传入 `max_subscribers=32`）
- `AlignmentTest` 两处断言逻辑错误（栈对齐假设 / `posix_memalign` 最小对齐要求）
- `write("string literal")` 类型推导为 `const char[N]` 导致的链接错误

### Removed
- `ISeqlock` / `Seqlock` / `SeqlockException` / `SeqlockFactory` / `SeqlockUtils` 类层次
- `ISubscriptionManager` 虚接口
- `src/osal/linux/` 目录（POSIX 重复封装）
- `src/osal/interface/` 目录（废弃 base 类）
- 各测试文件独立的 `main()` 函数（统一使用 `GTest::gtest_main`）

---

## [1.0.0] — 2026-03-27

### Added
- 初始项目脚手架
- 四层架构：Application / API / Core / OSAL
- 类型系统：`PointData` (128B), `InduRTDBHeader` (64B), `SubscriberEntry` (16B)
- OOP 风格 Seqlock 实现 (`ISeqlock` / `Seqlock` / `SeqlockFactory`)
- `PointManager` 虚接口 + `PointManagerImpl`
- `SubscriptionManager`（STL 容器实现）
- `SharedMemorySegment` 接口定义（空壳）
- `ConfigLoader` 接口定义（空壳）
- C++/C API stub
- POSIX OSAL 实现 (shm_open/mmap, Unix Domain Socket, pthread)
- Google Test 框架集成
- 需求文档 (SRS, 编码规范)
- 设计文档 (HLD, 工程框架架构, LLD)
- 技术文档 (Seqlock算法设计)
- 开发规划
