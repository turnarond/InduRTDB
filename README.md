# InduRTDB — Industrial Real-Time Database

**纯 C11 实现** 的工业实时数据库,面向边缘控制的超低延迟、多进程共享内存数据中枢。

## 定位与边界

**InduRTDB 是工业级"共享内存实时数据层"** —— 一个纯 C、跨进程、确定性的内存点位存储库。它是数据链路**中间的实时热数据层**，不是整条链路。

| 做 | 不做 |
|---|---|
| 跨进程共享内存实时点位存储 (低延迟、确定性) | 南向设备接入 (Modbus/OPC UA/S7 驱动) |
| 工业语义 (quality / timestamp / unit / access) | 北向 SCADA 对接 (OPC UA Server / MQTT 上送) |
| 多进程读写、订阅、崩溃自愈 | 持久化 / 历史库 |
| 作为库被集成 (find_package / pkg-config / FetchContent) | 集群 / 跨节点同步 |

**与 node-server 的关系:** InduRTDB 作为 [node-server](https://github.com/acoinfo/edge-framework)（BAS Edge Data Hub）的**底层数据层**,提供跨进程共享内存实时能力;北向 OPC UA、持久化等能力复用 node-server 既有实现,不重复造轮子。

### x86 实测性能 (v3.7.0 基线, Release -O2；v3.8 未改热路径，未复测)

| 操作 | P50 | P99 | 吞吐 |
|------|-----|-----|------|
| write_int32 | 0.247 μs | 0.255 μs | 26.1M op/s |
| write_double | 0.110 μs | 0.113 μs | 9.0M op/s |
| read_int32 | 0.042 μs | 0.044 μs | 23.6M op/s |
| read_double | 0.043 μs | 0.044 μs | 23.1M op/s |
| peek (单拷贝·线程本地) | 0.036 μs | 0.038 μs | 27.5M op/s |
| read_range (100点) | 0.234 μs | 0.271 μs | 84.4M pt/s |
| write_range (100点) | 1.398 μs | 1.433 μs | 14.2M pt/s |
| mixed_rw | -- | -- | 23.0M op/s |

> 设计目标 P99 ≤ 10 μs。上表为 **v3.9.0 复测基线**（x86_64 / Release `-O2`，2026-10-20，`run_bench.sh --quick`），P99 全部 PASS，与 v3.7/v3.8 数值在机器差异范围内一致、无显著退化。
> 详见 `tests/bench/bench.c` + `scripts/run_bench.sh`。

## 快速开始 (C)

```c
#include <indurtdb/indurtdb.h>

int main(void) {
    // 初始化 (单例)
    indurtdb_initialize("my_instance", 50000, 32);

    // 写入
    indurtdb_write_double(1001, 23.5);       // 温度
    indurtdb_write_bool(2001, true);         // 水泵

    // 读取
    indurtdb_point_t p;
    indurtdb_read_point(1001, &p);
    printf("温度: %.2f\n", p.value.d);

    // peek (单拷贝到线程本地缓冲, 下次 peek 覆盖; 长期持有请用 read_point)
    const indurtdb_point_t* pk = indurtdb_peek(1001);

    // 批量
    indurtdb_point_t buf[10];
    int n = indurtdb_read_range(0, 10, buf, 10);

    // 清理
    indurtdb_shutdown();
}
```

## 已知约束（使用前必读）

1. **`peek()` 是单拷贝，不是零拷贝**：拷贝到 `_Thread_local` 缓冲后返回其指针，同线程下一次 `peek()` 即覆盖；需长期持有请用 `indurtdb_read_point()`。
2. **单进程单例**：同一进程内只能持有一个实例，切换实例须先 `indurtdb_shutdown()`；不同 `instance_id` 的跨进程隔离正常。
3. **写冲突不重试**：多写者并发时若 Seqlock 处于写中状态，`write_*` 直接返回 `-2`（busy），不阻塞、不重试，调用方自行处理。
4. **超时检测 best-effort**：`check_timeouts()` 扫描遇写冲突会跳过该点位，单次调用不保证覆盖全部点位；**写入者以最大速率无间隙自旋时，扫描可能整轮取不到写锁而返回 0**（真实采集周期下不会触发，见 [issue #19](https://github.com/turnarond/InduRTDB/issues/19)）。
5. **订阅为进程内回调**：暂不支持跨进程变更通知（规划于 v3.3）。
6. **本库不含鉴权/加密/持久化/北向协议**：这些能力由 node-server 或上层 Bridge 承担。

完整边界与路线图见 [`docs/01-白皮书/01-产品白皮书.md`](docs/01-白皮书/01-产品白皮书.md)。

## 构建

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
ctest --output-on-failure
```

## 运维（rtdbd）

### 退出码

| 码 | 含义 | 场景 |
|---|---|---|
| 0 | 正常退出 | — |
| 1 | 通用 / 启动错误 | 参数解析、policy 加载失败等 |
| 2 | 配置校验失败 | `load_config` 失败、点位语义非法 |
| 3 | 共享内存损坏 / 初始化失败 | `indurtdb_initialize` 失败 |
| 4 | 启动自检失败 | 段头 `magic` / `version` 不通过 |

配置或自检失败退出时 **保留共享内存段**（不销毁已有点位与值），修正后重启即可恢复。
`--supervise` 模式下，supervisor 对**致命码 2 / 4 停止 respawn** 并透传退出码；
systemd 部署需配套 `RestartPreventExitStatus=2 4`。

### 可观测性

- `RTDBD_OP_HEALTH`（opcode 16）：运行时健康快照，**只读免鉴权**；v3.11 协议主版本为 **3**。
- `kill -USR1 <pid>`：把运行统计写入 `--stats-file`（默认 `/tmp/rtdbd.stats`）。
- 运行日志：`RTDBD_OP_GET_LOG`（128 条定长环形缓冲）。

### 背压

每连接定长 **1024** 个 40B 通用出站帧（零堆）。v3.11 为 10k 快照预留
**767 快照槽 + 256 实时槽 + 1 控制槽**：快照/`SNAPSHOT_DONE` 永不丢弃；实时帧满时优先
淘汰未发送的队首实时帧，否则丢弃本次实时帧，均计入 `notify_drop` / `notify_drop_live`。
值仍落库，GET/BATCH_READ 可读到。`SET_SUBS` 整组替换会软清旧队列（不计 `notify_drop`）。

## 版本

**v3.11.0（开发中）** — 单连接 1 万订阅与批量 I/O：

- **协议 v3**：新增 `BATCH_READ=19`、`BATCH_WRITE=20`、`CAPS=21`、`SNAPSHOT_DONE=22`；
  `NO_MEM=5` 表示有界分配失败。旧主版本客户端仍可用 ≤1024 订阅与单点 GET/WRITE。
- **容量**：单连接订阅上限 **10000**、连接池 **128**（默认预创建 32）、批量 **1024**；
  `rtdbd:` YAML 或 CLI 配置，CLI 优先。`max_points` / `instance` 仍只能 CLI。
- **10k 流式快照**：`SET_SUBS` 先排序去重，再按 767 帧/轮分批推送；每帧 `reserved[1]`
  置 `NOTIFY_F_SNAPSHOT`；已发送 `CAPS` 的客户端在最后收到 `SNAPSHOT_DONE`。
- **批量 I/O**：单请求最多 1024 点，逐项返回状态；允许重复 id，部分失败不回滚；
  请求/响应大小有硬上限，不合法包直接 `BAD_REQUEST`。
- **监控台/Python 客户端**：`probe_caps()` 协商 `max_subs`/`max_batch`；监听器识别快照
  标记与 `SNAPSHOT_DONE`；`batch_read()` / `batch_write()` 可用。旧服务端自动回退 1024。
- **规模矩阵**：`scripts/run_scale_matrix.py` 覆盖 2k/20k/200k/500k/1M × 1/5/10 Hz，
  输出 JSON/CSV 与原始日志；`run_station_load.sh` 默认按 10000 订阅启动。
- **升级注意**：v3.11 仍使用段版本 3，`indurtdb_point_t` 仍为 128B；旧 v3.9.x
  （订阅槽位 32）段仍不能直接 attach。旧客户端不发送 `CAPS`，不会收到 `SNAPSHOT_DONE`。

**v3.10.0** — 单站规模化：批量订阅、质量推送与分页监控：

- **`SET_SUBS`（opcode 18）**：单次最多 1024 点整组替换，先校验后生效；成功后按 id 升序推送当前值快照。
- **容量**：每连接订阅上限默认 **1024**、出站队列 **1024** 帧、连接表 **128**；段内订阅槽位固定 128。
- **质量与超时**：NOTIFY 携带 quality；`--timeout-ms` / YAML `timeout_ms` 由 rtdbd 1 Hz 扫描，Good→Timeout 只推一次，写恢复 Good。
- **监控台**：服务端分页（默认 50 点/页）、每个 WS 独立订阅、旧服务端自动降级 32 点/页。
- **负载脚本**：`scripts/run_station_load.sh` 覆盖 20k 点 × 1 Hz × 50 客户端 × 500 订阅，断言 `notify_drop == 0`。
- **升级注意**：旧 v3.9.x 段（订阅槽位 32）不能直接 attach；停 rtdbd 后删除 `/dev/shm/indurtdb_<instance>` 再启动。

**v3.9.1** — 监控台实时链路与时间戳修复（与 3.9.0 兼容，无协议/段/API 变更）：

- **修复 · 实时推送 2s 后静默失效**：监听线程不再把 socket 空闲超时当断链；前端收到清单后显式订阅。
- **修复 · `/api/logs` 泄露未初始化栈内存**：日志消息缓冲先清零，NUL 之后恒为 0。
- **修复 · 点位时间戳显示为 40432 年**：新增 `irt_time_realtime_ns()`，入库时刻改用墙钟 epoch；超时检测同钟，心跳仍用单调时钟。
- **修复 · 质量列全空**：`quality` 字符串/数字统一归一化，按 `INDURTDB_QUALITY_*` 显示 Good / Bad / Timeout 等及量程位。
- **修复 · 新建点位 ID 从 0 开始**：默认与最小 ID 改为 **1**（自动取下一个空闲 ID），REST 拒绝 `id < 1`；0 保留给配置/历史点位。
- **修复 · 错误响应返回 200**：统一改用 `JSONResponse`，创建/写入/读取失败恢复 400/401/404。
- **回归**：监控台 E2E 增加「静默 >2s 仍收到 update」，C 侧增加日志尾零与墙钟断言。

**v3.9.0** — 点位分区 / 命名空间：

- **段版本 3**：`IRT_SHM_VERSION` 2 → 3，`indurtdb_point_t` 偏移 120 新增具名字段 `uint32 partition`（`sizeof==128` 不变）；v2 段被 v3 库拒绝挂载。
- **分区索引**：`name→id` 索引键改为 `partition‖name`，同名点位可跨分区共存；默认分区 0 与 v3.8 语义等价。
- **策略 4 段**：统一 `<uid>:<partition|*>:<min_id>:<max_id>`，旧 3 段写法 fail-fast（退出码 2）。
- **API**：v2 句柄函数 50 → 55（名称域 `_in` ×4 + `set_partition`），公开 API 共 117（v1 62 + v2 55）。
- **协议 v3 + 工具链**：`rtdbd` v3、`irtcli --partition/select`、监控台 `partitions` 视图。
- **迁移工具**：`tools/irt_migrate` 扩展 v1→v3 与 v2→v3（分区清零、元数据保留、索引重建、CRC 重封、版本保护），并完成回退演练。

**v3.8.0** — 部署能力补齐 + 文档治理 + 缺陷修复：

- **修复 · CI 门禁恢复**：API 计数门禁（流水线第一步）期望 47+43 而实际 62+50，自 v3.7 主题A 起每次 push 均在 10 秒内失败，`v3.7.0` 的 tag 是在 CI 为红的状态下打的。已修正门禁基准与 SDK 手册首段计数。
- **修复 · 写路径错误码契约不符**：`h_write_and_notify` 原样透传 pm 层负值，内部 `-2`（写冲突）恰好等于对外 `ERR_NOT_FOUND`、`-3`（只读拒写）恰好等于 `ERR_FULL`。写路径**从不返回**头文件承诺的 `INDURTDB_ERR_BUSY(-5)`，只读拒写被误报为「索引已满」。已统一对齐并新增 `INDURTDB_ERR_READ_ONLY(-6)`。
- **修复 · 并发写丢写率 ~50%**：写路径遇 Seqlock 冲突直接返回、不重试。已加库内有限重试（2 线程丢写率降至 <2%，配截止时间重试后零丢失）。
- **修复 · 配置解析器不支持 int64/uint32**：YAML 中写 `type: int64` 被静默丢弃而 rtdbd 退出码仍为 0。
- **新增 · 部署能力**：`scripts/verify_deploy.sh`（18 项部署自检）、`scripts/check_rtdbd_health.py`（UDS 健康检查与告警基线）、`docs/07-部署运维/`（5 篇）、`config/rtdbd/`（官方配置样例，11 点位实测）。
- **新增 · 安装补齐**：`cmake --install` 现安装 `rtdbd` / `irtcli` / `irt_diag_harness` 及其头文件，`InduRTDB::irtcli` 可被 `find_package` 取用。
- **新增 · 文档**：`docs/05-SDK手册/05-v1到v2迁移指引.md`、`examples/peek_lifetime_example.c`（可运行的单拷贝陷阱复现）。
- **协作约定**：确立 release 分支制（`main` ← `release/<版本>` ← `feature`/`fix`，合入后删 feature/fix，版本分支长期保留，收尾打 tag）。

**v3.7.0** — 核心语义深化 + 运维硬化：

- **主题A**：死区/RBE 过滤 + EURange→量程位生效（语义纯函数公共 API、质量感知写入口、rtdbd 按订阅者 RBE 通知门控）。
- **主题B**：运维硬化（健康/错误计数 + `RTDBD_OP_HEALTH` + SIGUSR1 dump、每连接有界出站队列背压 + 优雅退出、配置校验 fail-fast + 语义退出码、`indurtdb_detach()` 保留段）。
- **主题B 延展 B4**：运行时配置持久化（delta 日志，`rtdbd --delta-file` 默认关闭，零 ABI 变更）。
- **主题B 延展 B5**：v1 单例 API 标注 `INDURTDB_DEPRECATED`（未删除，v4.0 移除）。

**v3.3.0** — 读写分离与双通道：新增 `rtdbd` 写权威守护进程（UDS 串行写 + 本机 UID 鉴权 + 审计）与 `indurtdb-client`（写队列，fail-operational），支持跨进程变更通知；点位新增 `source_timestamp_ns`（采集时刻）与 `COMM_FAILURE` 质量码。**共享内存布局与 v2.x 仍逐字节兼容，ABI 保持 v1。**

v3.1.0 — 纯 C11 重写,C++ API 已移除。共享内存布局与 v2.x 逐字节兼容。

## 文档

- [集成指南](docs/05-SDK手册/04-集成指南.md) — find_package / pkg-config / FetchContent 三种集成方式
- [完整文档目录](docs/README.md) — 白皮书、设计、API 参考等

## 许可证

MIT License
