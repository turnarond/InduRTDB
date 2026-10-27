# InduRTDB 集群同步设计（v3.12 提案）

**日期**: 2026-10-27
**状态**: DRAFT（待用户确认；依据为 5 条工作假设，见 §1.1）
**基线**: release/3.10 @ 35f565a4（v3.11 订阅+批量 IO 之后）
**版本号**: 独立功能特性设计（非修复性变更）

---

## 1 意图与为什么

用户需求（原文）：

> 现在想要做一个新功能，就是集群式的。可能配置两个及两个以上的数据库，支持全量和增量同步（两个也是有可能的）有变化的时候就会同步到对端。

解读：为 InduRTDB 增加"集群"能力——同一套配置下可声明 2 个及以上的数据库实例，实例之间支持**全量同步**（表结构 + 当前值快照）与**增量同步**（写变化实时/近实时传播），用户写任一实例的变化最终到达对端。

注意：`README.md` 当前把"集群 / 跨节点同步"列在**不做**列。本设计实质上是宣布对该边界的一次推翻与重做，需在 README 中同步更新（见 §7）。

### 1.1 用户已确认的约束

用户未逐条确认，以下为按行业惯例与仓库先例提出的 5 条**工作假设**，已随访谈发出且未收到否决，作为本文档基线：

1. **拓扑**：主备制（Primary–Backup）。支持 1 Master + 1..N Standby（N≥1，覆盖"两个也是有可能的"）。默认**单向**同步：Standby 对数据只读或只收，Master 为权威；双向同步/故障切换留作后续扩展。
2. **内容**：
   - 全量：点表结构 + 所有点当前值快照；
   - 增量：写路径变化（Write/Delta）+ 点表结构变化；
   - 触发：Master 端任何变化即推送到对端（"有变化的时候就会同步"）。
3. **传输**：优先复用现有 rtdbd 的 UDS/IPC 层（单机）；跨主机场景复用 edge-framework node-server 北向通道。**最终选择留给实现者**（§10）。
4. **冲突**：单向同步下不需要真冲突合并——Standby 冲突写兜底策略为"Master 权威，Standby 写入被拒绝并告警"；双向为后续特性。
5. **配置**：在 `rtdbd_conf` 上新增 `sync:` 配置段（对端地址/角色/全量触发条件等），校验沿用"仅警告不阻断"先例（对齐 `rtdbd_validate_config` 的 warn-only 风格）。

若上述任何一条与用户真实意图不符，请在确认时指出，本文档按修订更新。

## 2 目标 / 非目标 / 反目标

### 2.1 目标

- G1：配置 2..N 实例间的主备同步；standby 挂掉/新加入后可自动全量补齐。
- G2：Master 写路径上的变化以增量形式传播；正常稳态下写 TPS 损耗可控（门禁值见 §6）。
- G3：变化端到端可见延迟满足 §6 规定的场景门禁。
- G4：`sync:` 未配置时，进程行为与 v3.11 完全一致（默认关闭，纯增量特性）。
- G5：公开 API（`src/api` 的 `indurtdb_*` 头）**不变**；集群能力纯粹由 rtdbd 配置面驱动的进程间行为。

### 2.2 非目标（本次不做）

- N1：双向同步与正式故障切换（含脑裂仲裁）——只保留"权威方"单一写入主体。
- N2：跨集群/树形级联同步（A→B→C 链式传播）。
- N3：任意异构数据库同步（只同步 InduRTDB ↔ InduRTDB）。
- N4：sync 数据流的业务级过滤（初期全量转发，不做订阅过滤表达式下沉到同步面）。

### 2.3 反目标（明确不采用的方案）

- C1：**旁路轮询 SHM 扫描变更**。v3.11 已有 `irt_delta` + `irt_subscription` 的写路径钩子模型，任何"定时全表 diff"方案都会重扫 2.5M TPS 主路径并引入 O(N) 扫描，直接反目标。
- C2：在 `src/api` 公开头上加任何集群相关符号（违反"config 面行为驱动"的现有分层）。
- C3：Standby 端允许任意写后自动合并（无权威仲裁的最终一致性，不做）。

## 3 拓扑与容量模型

### 3.1 拓扑

```
                 ┌────────────┐
   写入 ────────►│  Master    │
   读分布        │  (rtdbd A) │
                 └─────┬──────┘
            sync 链路  │ 增量流 + 快照流
            ┌──────────┼──────────┐
            ▼          ▼          ▼
        Standby A  Standby B  Standby C
        (rtdbd B)  (rtdbd C)  (rtdbd D)
```

- Master 由 `sync.role: primary` 声明；同一 `sync` 配置组内的实例通过 `peers` 列表互认。
- 默认单向：Master → 所有 Standby。
- 两个实例即 N=1 Standby 的特例；协议不假设 N。

### 3.2 变化的来源（同步触发面）

Master 端触发点收敛为一个挂接位置：**写路径的 delta/版本钩子**（候选实现点：`src/core/irt_point_manager.c` 的版本更新路径与 `irt_delta_open_append/append` 调用链；`irt_subscription.h` 的 `irt_sub_notify` 通知链）。同步器订阅与既有订阅客户端同级，不引入新扫描线程。

同步三类事件：

| 事件 | 来源 | 同步形式 |
|---|---|---|
| 点值变化 | 写路径 delta 记录 | 增量 |
| 点表结构变化（加/删点） | 表定义变更路径 | 增量（先于值传播） |
| 全量快照 | standby 加入 / 恢复条件触发 | 快照 + 之后增量续流 |

### 3.3 容量模型

沿用已测基线：进程内 2.5M TPS（v3.10–v3.11 已验证），UDS 约 600K TPS。同步路径设计预算：

- 增量转发在 Master 进程内做**合并批处理**：以同步批次（按条数或时间窗 flush）打包，单条转发成本按批次均摊；
- delta 事件率应 ≪ 写 TPS（每写一条 log 一条 delta），转发吞吐按"delta 事件/秒 = 写 TPS"打折估算；600K UDS 通道对 100K 级增量事件/秒有充足余量（待 §6 实测确认阈值）；
- 若 delta 流积压超过阈值（§4.7），**不扩缓冲硬扛**，而是放弃增量、转全量快照——保持"unque 空间恒定"的一贯纪律（对齐 outq 反压/`conn_discard_outq` 先例），积压不可恢复时用快照兜底而非无限内存。

### 3.4 配置模型

```yaml
sync:
  enabled: true
  role: primary              # primary | standby
  transport: node            # node(edge-framework) | uds(同机原子实例) —— §10 裁定
  peers:                     # primary: 对端地址列表；standby: 指向 master
    - name: standby-1
      addr: "tcp://10.0.0.2:7800"
  full_sync:
    on_join: true            # standby 上线/重连即全量
    interval_s: 0            # 0 = 除重连外不周期全量
  delta:
    flush_ms: 20             # 增量批次 flush 窗
    lag_fullsync: 100000     # 未确认增量数超过则转全量
```

- 校验规则（纯警告、不阻断，对齐 `rtdbd_validate_config` warn-only 先例）：
  - `role=standby` 但谁的地址都不指向 primary → warn；
  - 两个实例同时声明 `primary` 且互指 peer → warn（不动成环检测的完整性，弱网下允许普通成对声明）；
  - `enabled` 时开启幂等要求（见 §4.9）。

CLI：

```sh
rtdbd --config rtdbd.yaml          # 原有；sync: 段落存在即启用同步
rtdbd --sync-status                # 查询链路状态（连接、lag、full_sync 进度）
```

（`--sync-status` 为观测面非数据面，独立小参数，不进公共 C API。）

### 3.5 进程模型

- 同步器置于 **rtdbd 主进程**内（与 conn pool、outq 同层），一个 Master 进程管理 N 个 peer 连接（一 peer 一连接）；
- 复用现有 shutdown 顺序（drain → 连接清理 → 关监听/句柄）；同步 socket 关闭置于 shutdown 早期，避免退出期残包。

## 4 同步协议设计

### 4.1 组件划分

| 组件 | 位置 | 职责 |
|---|---|---|
| SyncMgr | `rtdbd/src/`（新文件，如 `sync.c/h`） | 链路管理、状态机、批次收发 |
| HookBridge | 写路径钩子旁挂 | `irt_delta_apply`/`irt_sub_notify` 转 sync 队列（进程内无锁队列） |
| Snapshotter | Master 侧 | 全量快照序列化（points + 值），走同一条 transport |
| Applier | Standby 侧 | 重放：结构→快照→增量（顺序详见 §4.7） |

### 4.2 全量同步（快照流）

时机：standby 加入（`on_join: true`）、重连且增量不可续传、`full_sync.interval_s` 到期。

内容与顺序（**冻结顺序**，Standby 侧还原时严格按此序应用）：

```
1. points 定义帧       （全部点：id/类型/量纲/policy）
2. 值快照帧            （每点当前值 + seq）
3. ready 帧           （Master 宣布快照底座 LSN——快照排产时序号）
```

序列化基于现有 `aof` / 增量文件的复用优先级：`irt_point_manager` 已有打开"基线"概念（B4: base → delta replay → validate → self_check → service），快照格式**直接对齐该基线文件格式**，Achieve 一处实现两处复用（standby 加载即执行同一条 base+replay 路径）。

### 4.3 增量同步（delta 流）

- Master：HookBridge 把写路径 delta 记录转 push 到 SyncMgr 队列；SyncMgr 按 `flush_ms` 批量组帧：
  ```
  delta 帧 = [ base_lsn | count | rec_1 ... rec_count ]
  rec      = 现有 irt_delta_rec_t 直传（不重编码）
  ```
- Standby：收帧后逐一 `irt_delta_* apply`（重用 `irt_delta_apply_fn`）；确认机制按"应用即 ack"，Master 每 K 帧记录远端水位。
- 表结构变化帧先于其后的值变化帧（Master 端保证入队顺序；同一条 `irt_delta` 流中结构变更是定点事件）。

### 4.4 状态机（每 peer）

```
DISCONNECTED ──connect──► CONNECTING ──ok──► HANDSHAKE
HANDSHAKE ──配对成功──► FULL_SYNC（发送/接收快照）
FULL_SYNC ──ready──► STEADY（增量流）
STEADY ──lag 超限 / 断链 / 结构大改──► FULL_SYNC
任何态 ──err──► DISCONNECTED（指数退避重连，1s/2s/4s/…/max 30s）
```

### 4.5 底座一致性约束（核心）

**增量必须只认"同一底座"**：

- `base_lsn`（快照底座时序号）是每条 delta 的前提；
- Standby 端收到的任意 delta 帧 `base_lsn` 与本地"已应用底座 LSN"不一致 → 丢弃该帧之后全部流，立即回 `FULL_SYNC`；
- Master 发送窗口内一旦触发快照重排产，当前增量远窗口落判失效 → 转 FULL_SYNC。

这保证"standby 挂掉丢包重连"、"全量与增量竞态"都归拢为同一条规则，不增设补洞机制。

### 4.6 传输选型（两案并存，终选交实现者）

| 方案 | 适用 | 优点 | 代价 |
|---|---|---|---|
| **A: 复用 UDS/IPC** | 同机原子实例/单机多实例 | 零新依赖；复用 conn pool/outq 模式 | 不覆盖跨主机；多实例并存会撞现有 `sock_path` 固定 + `unlink(sock_path)` 的退出清理（需按实例名区分路径，牵动清理顺序） |
| **B: edge-framework node-server 北向** | 跨主机（主场景） | 现成可靠通道、已经过生产检验 | 引入外部框架依赖；带宽/打包需求在自己的帧格式上封装 |

**当前倾向**：默认 B（node-server），同机场景允许 A。协议帧格式与 transport 无关（transport 只承载字节流），保证可切换。

### 4.7 故障恢复

- 断链重连：指数退避；重连后先握手校验"点表结构 hash + 当前底座 LSN"，一致 → 续增量；不一致或 lag 超限 → 全量。
- lag 检查：`确认滞后条数 > lag_fullsync` → 强制 FULL_SYNC（§3.3 的纪律落地）。
- Master 崩溃恢复：Master 重启后自己按 B4 语义恢复内存态；对 standby 而言相当于一次可能底座变化的"重连"，统一走 §4.5 规则。

### 4.8 顺序一致性

单 peer 链路上 delta 顺序 = Master 写路径顺序（`irt_seqlock` 版本号天然单调），保证：

- 值序列（对同一点）standby 观测是 monotone 的；
- 结构变化先于其值；同 batch 内不重排。

### 4.9 幂等

- delta 帧带 `base_lsn` + 每条 `rec.lsn`（沿用 delta 记录内序），standby 端 dedupe：`rec.lsn ≤ applied_lsn` 直接跳过；
- 全量 resume：Standby 记录"已收帧偏移 + base_lsn"，重连时若槽位仍有效可从偏移续传（可选项，初版可不做、整段重发）。

### 4.10 Standby 读写策略

- 初版：**Standby 只读**——Standby 侧写入口在 sync 开启时收到"拒绝 + warn 日志"；
- 读完全放行（standby 是读扩展的合法用途，也正是"写分担"假设下主备制的兜底解释）；
- 双向同步/切换后的主从辩别（谁是 Master）留待后续版本，配置上今天不做。

## 5 为什么增量快、边界在哪

- **快**：同步路径不扫 SHM、不 diff 全表。每一笔变化是写路径上**一次 log**（`irt_delta_append` O(1) 摊还），同步面只是把日志流转发出去 + ack。与"订阅客户端"同一条变化链路，不增加写路径额外内存序/锁；批处理（count 帧）把网络往返摊薄到每 `flush_ms` 一次。
- **边界 1（CPU/带宽）**：standby 端是重放成本——它要真的执行写入，不共享 Master 的 SHM；大批量结构变更或冬天全量时 standby 会成为落盘端瓶颈，ہن快照帧必须分批流式（不能整段加载）。
- **边界 2（LSN 窗口）**：增量可用的前提是"Standby 的底座没变过去"。一旦全量重排产，窗口作废——**没有无限回放**，这是反压→全量兜底纪律的根源。
- **边界 3（跨进程语义）**：时钟不参与排序（跳变不安全），排序只看 LSN/seq。

## 6 性能测试规格

以下均为"实现者需给出实测数字 + 测法"的门禁，**具体目标值以实现者实测为准**，方向性预评为虚线：

- T1（写 TPS 损耗）：Master 开 sync 单 peer 1K ops/s 增量负载 vs 关 sync —— 预期损耗 ≤ 某百分比（虚线 5%），场景：10K 点 / 100K 点 / 2.5M 点 表。
- T2（端到端延迟）：写后 standby 可见延迟分位数（p50/p99），flush=20ms 帧窗下预期 p99 ≪ 100ms 量级（实测为准）。
- T3（全量时间）：2.5M 点全量从 Master → Standby 的墙钟时间，要求不出进程 OOM（流式分批）。
- T4（lag 兜底触发率）：压测注入断网 30s 再恢复，统计"转全量"的触发比例；
- T5（同机两实例冲突路径）：`role=primary` + 同机 UDS 方案 A 路径跑 T1–T3 基础一遍（可裁掉 T4）。

测试实现载体：优先在 rtdbd 现有 bench/测试 harness 中挂"standby 角色模式"，不新建独立压测仓库。

## 7 兼容性

- 默认零行为变更：`sync:` 不配置时，代码路径 `if (!cfg.sync.enabled) skip` 全部旁路，无额外线程/句柄。
- 公开 API 不变（G5）。
- 配置文件向后兼容：`sync:` 是新增可选段，无 schema 破坏。
- **多实例同一主机共存**：现有 rtdbd 固定 `sock_path` + 退出 `unlink(sock_path)` —— 一旦同机跑两个 rtdbd（方案 A 或调试），必须改成"实例名派生路径 + 退出只 unlink 自己的路径"；这部分改动即使选 B 传输也存在（IPC 面），需与方案解耦地做（`§10 留给实现者`）。
- README 声明更新：把"集群 / 跨节点同步"从不做列移到"有限支持（主备单向）"。

## 8 风险与回退

| 风险 | 缓解 | 回退 |
|---|---|---|
| 写路径挂钩引入新延迟 | 挂钩为线程内队列 push（非阻塞、有界丢弃备选） | 降级为"关 sync 即旁路" |
| delta 流积压致内存增长 | `lag_fullsync` 阈值 → 全量兜底（反压纪律） | 无需特殊回退， behaviour 本身即回退路径 |
| Standby 重放堆积 | 归入 lag 量度；Applier 逐条 apply 但不阻塞收帧线程 | 缩 lag 阈值 |
| edge-framework 依赖引入不稳 | 帧格式与 transport 解耦；A 方案保留同机回退路径 | 切 A（同机）/暂停功能（跨机） |
| 结构变更与值并发竞态 | 结构事件先于值、同一序号空间 | 出问题直接全量（§4.5） |
| README"不做"声明误导用户 | §7 明确改声明 | — |

回退总原则：**整个功能为纯增量，回退 = 从配置中删去 `sync:` 段**，不牵联主路径。

## 9 测试与门禁

- 单元：
  - 配置解析（含 warn 校验用例：双 primary、standby 乱指、缺 addr）；
  - 帧编解码 roundtrip（结构帧/快照帧/delta 帧/ready 帧）；
  - 幂等 dedupe（同 ack 重发、乱序、`lsn` 回退）。
- 集成（必须，走现有测试框架）：
  - **IR1 全量补齐**：standby 加入 → 全量 → 与 Master 逐点逐值 diff 为零；
  - **IR2 增量一致**：稳态写 N 点 M 条后停写 → standby 最终一致；
  - **IR3 断链恢复**：kill -9 standby → 重连 → 底座一致续增量（不转全量）或底座漂移正确转全量；
  - **IR4 lag 兜底**：人为拔线超过 `flush` 堆积 → 恢复后转全量且最终一致；
  - **IR5 standby 保护**：standby 上写被拒绝且有 warn 日志。
- 门禁：IR1–IR5 全绿 + §6 的 T1/T3 实测数字落库。★★★ 标准（数据面行为变更必须带实测）。

## 10 已裁定 / 留给实现者

### 已裁定

- 主备单向拓扑，Standby 初版只读；
- 触发点收敛在写路径 delta/订阅钩子，**不做旁路扫描**（反目标 C1）；
- 增量与全量统一用"底座 LSN"一致性规则，不建补洞机制（§4.5）；
- delta 帧封装既有 `irt_delta_rec_t`，不二次编码；
- 公开 API 不变，能力纯由配置面驱动；
- 校验 warn-only，不做阻断式 schema 检查；
- 全量回退/兜底用反压→FULL_SYNC 纪律，非无限缓冲。

### 留给实现者

1. **传输终选**（方案 A vs B）：倾向 B（node-server）；若同机调试需要 A，可先交同机 UDS 的最小实现。**决策前需测 node-server 引入后的启动/稳定性成本**。
2. **写路径挂钩的精确落点**：`irt_point_manager` 版本更新路径 vs `irt_sub_notify` 下游旁挂——按"最小锁影响 + 最低入队延迟"实测选点（候选已列 §3.2）。
3. **同机多实例的 `sock_path` 改造**：实例名派生路径 + 退出只 unlink 自己（§7），作为方案 A 的前置或者独立必修项。
4. **门禁具体数值**（T1 损耗上限、T3 全量时间）：实现者以实测校准后冻结为常量。
5. **快照 resume**（可选项）：初版全量整段重发，如 T3 数字不能接受再加续传。
6. **结构变更事件在现有 delta 流中的精确编码**（当前 `irt_delta_rec_t` 是否含结构事件类型，需实现时确认；若无则扩展一个事件 kind，向前兼容）。

---

*EOF — 2026-10-27, brainstorming 阶段产物；实现计划另行通过 writing-plans 流程产出。*
