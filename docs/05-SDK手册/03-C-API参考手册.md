# InduRTDB C API 参考手册

**版本**: 3.4.0 | **更新日期**: 2026-10-04 | **变更**: v3.4 布局 v2 + API v2（句柄化 / 按名查找 / 元数据 / 类型扩展 / 质量 OPC UA 映射）。公开 API 共 **90 个**：v1 全局函数 47 + v2 句柄函数 43。

**头文件**: `<indurtdb/indurtdb.h>`
**链接**: `-lindurtdb -lpthread -lrt`

---

## API v2：句柄化（v3.4 新增）

v3.4 起支持**去单例化**：同一进程可同时持有多个实例，各自读写互不干扰。

- `indurtdb_t`：不透明句柄（定义于库实现）。
- `indurtdb_h_open(&h, instance_id, &cfg)` / `indurtdb_h_close(h)`：打开 / 关闭实例。
- v2 函数一律加 `_h_` 前缀（C 无重载），入参首位为 `indurtdb_t* h`，返回**语义化错误码**（见下）。
- v1 的 47 个全局函数**全部保留**，行为不变，等于默认句柄 `g_default` 的薄封装 —— 既有调用方**零改动**即可升级到 v3.4。v1 在 v3.5 起标注 `deprecated` 并最终移除。

### 多实例示例

```c
indurtdb_cfg_t cfg = { .max_points = 64, .max_subscribers = 4 };
indurtdb_t *a = NULL, *b = NULL;
indurtdb_h_open(&a, "plantA", &cfg);
indurtdb_h_open(&b, "plantB", &cfg);

indurtdb_h_write_int32(a, 1, 100);   /* 写 A */
indurtdb_h_write_int32(b, 1, 200);   /* 写 B, 不污染 A */
int32_t v;
indurtdb_h_read_int32(a, 1, &v);     /* v == 100 */
indurtdb_h_close(a);
indurtdb_h_close(b);
```

### 语义化错误码（v3.4 起部分接口返回）

| 码 | 值 | 含义 |
|---|---|---|
| `INDURTDB_OK` | 0 | 成功 |
| `INDURTDB_ERR_ARG` | -1 | 参数非法（空指针 / 越界 id / 空名 / max_points=0） |
| `INDURTDB_ERR_NOT_FOUND` | -2 | 按名未找到 |
| `INDURTDB_ERR_FULL` | -3 | 索引/表满，或实例槽位耗尽 |
| `INDURTDB_ERR_NOT_INIT` | -4 | 实例未初始化（仅 v2 `_h_*` 返回；v1 仍返回 -1） |
| `INDURTDB_ERR_BUSY` | -5 | 并发写冲突，重试耗尽 |

> v1 接口向后兼容：只判 `< 0` 的旧代码行为完全不变；`ERR_NOT_INIT` 对 v1 被映射回 `-1`。

### v2 句柄函数完整清单（共 43 个）

所有 `indurtdb_h_*` 与对应 v1 全局函数**签名同构**（仅入参首位多 `indurtdb_t* h`），返回**语义化错误码**（见上方错误码表）。下表为全量收录，与 `indurtdb.h` 公开 API 一一对应。

| v2 句柄函数 | 对应 v1 | 类别 |
|---|---|---|
| `indurtdb_h_open` / `indurtdb_h_close` / `indurtdb_h_is_initialized` | `indurtdb_initialize` / `indurtdb_shutdown` / `indurtdb_is_initialized` | 生命周期 |
| `indurtdb_h_write_bool` / `h_write_int32` / `h_write_double` / `h_write_string` | `indurtdb_write_bool/int32/double/string` | 写·单点 |
| `indurtdb_h_write_int64` / `h_write_uint32` / `h_write_float` | `indurtdb_write_int64/uint32/float` | 写·单点（v3.4 类型扩展） |
| `indurtdb_h_write_bool_ts` / `h_write_int32_ts` / `h_write_double_ts` / `h_write_string_ts` | `indurtdb_write_*_ts` | 写·携带采集时刻 |
| `indurtdb_h_write_int64_ts` / `h_write_uint32_ts` / `h_write_float_ts` | `indurtdb_write_int64_ts/uint32_ts/float_ts` | 写·携带采集时刻（v3.4 类型扩展） |
| `indurtdb_h_read_bool` / `h_read_int32` / `h_read_double` / `h_read_string` / `h_read_point` / `h_peek` | `indurtdb_read_bool/int32/double/string/read_point/peek` | 读·单点（含 peek） |
| `indurtdb_h_read_int64` / `h_read_uint32` / `h_read_float` | `indurtdb_read_int64/uint32/float` | 读·单点（v3.4 类型扩展） |
| `indurtdb_h_read_range` | `indurtdb_read_range` | 批量读 |
| `indurtdb_h_write_range_bool` / `h_write_range_int32` / `h_write_range_double` | `indurtdb_write_range_bool/int32/double` | 批量写 |
| `indurtdb_h_subscribe` / `h_unsubscribe` | `indurtdb_subscribe` / `indurtdb_unsubscribe` | 订阅 |
| `indurtdb_h_load_config` / `h_set_quality` / `h_update_heartbeat` | `indurtdb_load_config` / `indurtdb_set_quality` / `indurtdb_update_heartbeat` | 配置/心跳 |
| `indurtdb_h_find_by_name` | `indurtdb_find_by_name` | 索引（v3.4） |
| `indurtdb_h_get_meta` / `h_set_meta` | `indurtdb_get_meta` / `indurtdb_set_meta` | 元数据（v3.4） |
| `indurtdb_h_write_quality_ts` / `indurtdb_write_quality_ts` | `—` | 写·质量感知（写权威，v3.7 主题A） |
| `indurtdb_value_to_double` / `indurtdb_eurange_limit` / `indurtdb_deadband_exceeded` | `—` | 语义纯函数（v3.7 主题A，可单测） |
| `indurtdb_h_check_timeouts` | `indurtdb_check_timeouts` | 校验 |
| `indurtdb_h_get_write_count` / `h_get_timeout_count` / `h_get_scan_skipped` | `indurtdb_get_write_count` / `get_timeout_count` / `get_scan_skipped` | 统计 |
| `indurtdb_h_validate_id` | `indurtdb_validate_id` | 校验 |

> v2 句柄函数同样支持 `indurtdb_cfg_t { max_points, max_subscribers }`（`indurtdb_h_open` 第三参）。多实例下 `g_default` 与 `h_open` 创建的句柄互不干扰。

---

## 类型定义

### indurtdb_point_t

```c
typedef struct {
    union {
        bool    b;
        int32_t i;
        double  d;
        char    str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t  type;       // INDURTDB_TYPE_BOOL(0) / INT32(1) / DOUBLE(2) / STRING(3)
    uint8_t  quality;    // INDURTDB_QUALITY_GOOD(0) / BAD(1) / TIMEOUT(2) / SUBSTITUTED(3)
    uint16_t unit;
    uint8_t  access;     // INDURTDB_ACCESS_READ_ONLY(1) / READ_WRITE(3)
    char     name[64];
    uint8_t  padding[19];
} __attribute__((packed, aligned(128))) indurtdb_point_t;
```

`sizeof` = 128 字节。与 v2.x `PointData` 共享内存布局逐字节兼容。

### 类型常量

```c
#define INDURTDB_TYPE_BOOL     0
#define INDURTDB_TYPE_INT32    1
#define INDURTDB_TYPE_DOUBLE   2
#define INDURTDB_TYPE_STRING   3

#define INDURTDB_QUALITY_GOOD        0
#define INDURTDB_QUALITY_BAD         1
#define INDURTDB_QUALITY_TIMEOUT     2
#define INDURTDB_QUALITY_SUBSTITUTED 3

#define INDURTDB_ACCESS_READ_ONLY   1
#define INDURTDB_ACCESS_READ_WRITE  3
```

### 回调类型

```c
typedef void (*indurtdb_callback_t)(uint32_t id,
    const indurtdb_point_t* data, void* user_data);
```

---

## 已知行为约束（使用前必读）

| # | 约束 | 说明 |
|---|---|---|
| 1 | **单进程单例** | 同一进程内 `indurtdb_initialize()` 只能持有一个实例；切换实例必须先 `indurtdb_shutdown()`。不同 `instance_id` 的跨进程隔离正常。 |
| 2 | **`peek()` 为单拷贝** | 数据经 Seqlock 拷贝到 `_Thread_local` 缓冲后返回其指针，**同线程下一次 `peek()` 即覆盖**；需长期持有请用 `indurtdb_read_point()` 拷贝到自管理缓冲。 |
| 3 | **写冲突不重试** | 多写者并发时，若 Seqlock 处于写中状态，`write_*` 立即返回 `-2`（busy），**不阻塞、不自旋重试**；只读点位返回 `-3`。调用方需按业务策略自行重试（该语义已在 v3.3 确定化）。 |
| 4 | **超时检测 best-effort** | `indurtdb_check_timeouts()` 扫描时若遇写冲突会跳过该点位, 单次调用不保证覆盖全部点位; 周期性调用即可收敛。**严苛边界**: 写入者以最大速率**自旋**(无间隙连续写)时, 全局单 seqlock 的可用窗口极短, 扫描可能整轮取不到写锁而返回 `0`; 真实采集周期(ms 级)下不会触发, 见 [issue #19](https://github.com/turnarond/InduRTDB/issues/19)。 |
| 5 | **订阅为进程内回调** | 回调仅在**本进程**内注册的订阅者上触发，**不支持跨进程变更通知**（核心库 `indurtdb_subscribe` 仅进程内；跨进程经 v3.3 的 `rtdbd`/`irtcli` 订阅事件实现）。 |
| 6 | **无鉴权/加密/持久化（库级）** | 核心库不提供网络服务、认证、加密与持久化；`access` 字段是静态只读标记，不是访问控制机制。这些能力由 node-server / 上层 Bridge 承担。v3.4 起 `rtdbd` 守护进程的**管控通道**（`SET_META` 元数据写）走本机 `SO_PEERCRED` 的 UID 级鉴权（deny by default），属**进程级**管控，非库级网络鉴权；跨主机 / 传输加密仍不在范围内。 |
| 7 | **rtdbd 管控策略无热加载** | `rtdbd` 的 UID 写策略（`--config` / `-p` 指定的策略文件）**仅在进程启动时加载一次**；运行时修改策略文件不会即时生效，须**重启 rtdbd** 方能应用新规则。当前版本不提供策略热加载 / SIGHUP 重载。 |

---

## 生命周期

### indurtdb_initialize

```c
int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points, uint32_t max_subscribers);
```

| 参数 | 说明 |
|------|------|
| `instance_id` | 实例名 (段名 `/indurtdb_<id>`) |
| `max_points` | 最大点位数 |
| `max_subscribers` | 最大订阅者数 |

**返回值**: 0 成功, -1 失败 (调用 `indurtdb_get_last_error()` 获取详情)。

首个调用的进程成为 owner 并初始化共享内存段。后续进程通过相同 instance_id 附加到已有段。

### indurtdb_shutdown

```c
void indurtdb_shutdown(void);
```

释放资源。Owner 进程同时删除共享内存段 (`shm_unlink`)。

### indurtdb_detach（v3.7 主题B B3）

```c
void indurtdb_detach(void);
```

释放资源但**保留共享内存段**（不 `shm_unlink`）。

与 `indurtdb_shutdown()` 的区别**仅在是否删除段**：两者都会 unmap 并复位句柄。

**何时用**："放弃接管但必须保留数据"的场景——典型是 `rtdbd` 的 fail-fast 退出。若此处用
`indurtdb_shutdown()`，owner 语义会删除整段，把「配置写错」升级为「运行时注册的点位与已
写入的值一并销毁」。改用 `indurtdb_detach()` 后，下一次启动仍可 attach 原段，由运维修正
问题即可恢复。

### indurtdb_is_initialized

```c
bool indurtdb_is_initialized(void);
```

查询单例是否已初始化。

---

## 写入函数 (单点)

所有写入: 成功返回 0, ID 越界返回 -1, 写冲突返回 -2。

写入后自动通知该点位的所有订阅者。

### indurtdb_write_bool

```c
int indurtdb_write_bool(uint32_t id, bool value);
```

`type` 自动设为 `INDURTDB_TYPE_BOOL(0)`。

### indurtdb_write_int32

```c
int indurtdb_write_int32(uint32_t id, int32_t value);
```

`type` 自动设为 `INDURTDB_TYPE_INT32(1)`。

### indurtdb_write_double

```c
int indurtdb_write_double(uint32_t id, double value);
```

`type` 自动设为 `INDURTDB_TYPE_DOUBLE(2)`。

### indurtdb_write_string

```c
int indurtdb_write_string(uint32_t id, const char* value);
```

`type` 自动设为 `INDURTDB_TYPE_STRING(3)`。字符串截断至 31 字符。

### indurtdb_write_int64 / write_uint32 / write_float（v3.4 类型扩展）

```c
int indurtdb_write_int64(uint32_t id, int64_t value);
int indurtdb_write_uint32(uint32_t id, uint32_t value);
int indurtdb_write_float(uint32_t id, float value);
```

新增整型 / 浮点类型写入（`INDURTDB_TYPE_INT64(4) / UINT32(5) / FLOAT(6)`），复用既有 32B 联合体，**布局不变**（结构体仍为 128B / `align(128)`）。

### indurtdb_write_int64_ts / write_uint32_ts / write_float_ts（v3.4 类型扩展）

```c
int indurtdb_write_int64_ts(uint32_t id, int64_t value, uint64_t source_ts_ns);
int indurtdb_write_uint32_ts(uint32_t id, uint32_t value, uint64_t source_ts_ns);
int indurtdb_write_float_ts(uint32_t id, float value, uint64_t source_ts_ns);
```

同上三类类型的「携带采集时刻」变体，`source_ts_ns` 语义与 `write_*_ts` 一致。

---

## 读取函数 (单点)

### indurtdb_read_bool

```c
int indurtdb_read_bool(uint32_t id, bool* value);
```

### indurtdb_read_int32

```c
int indurtdb_read_int32(uint32_t id, int32_t* value);
```

### indurtdb_read_double

```c
int indurtdb_read_double(uint32_t id, double* value);
```

### indurtdb_read_string

```c
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);
```

| 参数 | 说明 |
|------|------|
| `buffer` | 用户提供的输出缓冲区 |
| `buffer_size` | 缓冲区大小 (建议 >= 32) |

### indurtdb_read_int64 / read_uint32 / read_float（v3.4 类型扩展）

```c
int indurtdb_read_int64(uint32_t id, int64_t* value);
int indurtdb_read_uint32(uint32_t id, uint32_t* value);
int indurtdb_read_float(uint32_t id, float* value);
```

对应 `INDURTDB_TYPE_INT64(4) / UINT32(5) / FLOAT(6)` 的读取。

### indurtdb_read_point

```c
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
```

通过 Seqlock 协议读取完整点位数据 (128 字节安全拷贝)。

### indurtdb_peek (单拷贝·线程本地缓冲)

```c
const indurtdb_point_t* indurtdb_peek(uint32_t id);
```

通过 Seqlock 协议将点位数据**单拷贝**到线程本地缓冲后返回其指针，同线程下次 `peek` 覆盖。ID 无效时返回 NULL。

**约束**: 调用方不应长期持有此指针。跨写入边界时数据可能变化。适合高频读取和批量遍历场景。

---

## 批量读写

### indurtdb_read_range

```c
int indurtdb_read_range(uint32_t start_id, uint16_t count,
                        indurtdb_point_t* out_buf, uint16_t out_cap);
```

从 `start_id` 开始连续读取 `count` 个点位。返回实际读取点数, 负值表示参数错误。

### indurtdb_write_range_bool

```c
int indurtdb_write_range_bool(uint32_t start_id, const bool* values, uint16_t count);
```

### indurtdb_write_range_int32

```c
int indurtdb_write_range_int32(uint32_t start_id, const int32_t* values, uint16_t count);
```

### indurtdb_write_range_double

```c
int indurtdb_write_range_double(uint32_t start_id, const double* values, uint16_t count);
```

从 `start_id` 开始连续写入 `count` 个同类型值。返回实际写入点数, 遇到错误提前返回当前已写入数。

---

## 订阅

### indurtdb_subscribe

```c
int indurtdb_subscribe(uint32_t id, indurtdb_callback_t cb, void* user_data);
```

注册点位变更回调。当该点位被写入时, 回调在**写入者线程内**同步执行。

**约束**: 最多 256 个订阅槽位。回调中不得执行耗时操作或嵌套写入。**订阅仅在本进程内生效**——回调不会跨进程触发（跨进程变更通知已在 v3.3 经 `rtdbd`/`irtcli` 实现）。

### indurtdb_unsubscribe

```c
int indurtdb_unsubscribe(uint32_t id);
```

取消点位订阅。

---

## 配置与心跳

### indurtdb_load_config

```c
int indurtdb_load_config(const char* config_path);
```

从配置文件加载实例参数 (instance_id, max_points, max_subscribers) 并自动调用 `indurtdb_initialize()`。

配置文件格式:
```
instance_id=hvac_system
max_points=10000
max_subscribers=32
```

### indurtdb_update_heartbeat

```c
void indurtdb_update_heartbeat(void);
```

更新当前进程在心跳表中的时间戳。订阅者进程应定期调用 (建议间隔 <= 500ms), 以便 Owner 清理僵尸订阅者。

---

## 校验与统计

### indurtdb_validate_id

```c
int indurtdb_validate_id(uint32_t id);
```

检查 id 是否在 `[0, max_points)` 范围内。返回 1 有效, 0 无效。

### indurtdb_find_by_name

```c
int indurtdb_find_by_name(const char* name, uint32_t* out_id);
```

**v3.4 新增**。按点位名查找 id —— 索引位于共享内存内, 对同段的所有进程**全局一致**, 集成方无需各自维护"名字→id"映射层。

| 返回值 | 含义 |
|---|---|
| `INDURTDB_OK` (0) | 找到, `*out_id` 为点位 id |
| `INDURTDB_ERR_NOT_FOUND` (-2) | 该名字未注册 |
| `INDURTDB_ERR_ARG` (-1) | 参数非法（空指针 / 空名） |
| `INDURTDB_ERR_NOT_INIT` (-4) | 未 `indurtdb_initialize` |
| `INDURTDB_ERR_BUSY` (-5) | 并发写冲突且重试耗尽（重试即可） |

**约束**:
- 点位名须先注册才会被索引 —— 当前注册途径为 `indurtdb_load_config()`（v3.4 T9 起 `rtdbd` 亦负责注册）。
- 索引为**定长开放寻址表**（桶数 = `roundup_pow2(max_points × 2)`, 8B/槽, 负载因子 ≤ 0.5）, 随段一次性预分配, **无堆分配**; 段内内存开销约为 `8B × 桶数`（如 10000 点约 256KB）。
- 注册/注销走既有全局 seqlock（**注册期集中写**）, 查找走无锁读重试, **不进入读写热路径**。
- 未初始化时查找返回 `INDURTDB_ERR_NOT_INIT`, 不会创建段。

---

### indurtdb_get_meta / indurtdb_set_meta

```c
int indurtdb_get_meta(uint32_t id, indurtdb_meta_t* meta);
int indurtdb_set_meta(uint32_t id, const indurtdb_meta_t* meta);
```

**v3.4 新增**。读写点位**元数据**（每点 32B 冷数据）：工程量程 `eur_min / eur_max`、变化阈值 `deadband`、启用位 `flags`（bit0=量程, bit1=死区）。

| 返回值 | 含义 |
|---|---|
| `INDURTDB_OK` (0) | 成功 |
| `INDURTDB_ERR_ARG` (-1) | 越界 id / 空指针 |
| `INDURTDB_ERR_NOT_INIT` (-4) | 未 `indurtdb_initialize` |
| `INDURTDB_ERR_BUSY` (-5) | 并发写冲突且重试耗尽（重试即可） |

**约束**:
- 元数据是**参数存储**，库**只存不执行**：量程/死区不被 RTDB 主动用于过滤或上送，消费方（如 `rtdbd`）自行据此处理。
- `set` 走全局 seqlock 写锁（**注册/配置期**），`get` 走无锁读重试；**两者都不进入读写热路径** —— 频繁的点位值读写不会触碰元数据区。
- 结构 `indurtdb_meta_t` 为 **32B**（`align(32)`），字段偏移已静态断言锁死；升级/跨语言互操作时须保持逐字节兼容。
- 读/写前应确保 id 对应的点已注册（配置加载），否则数据无意义；未初始化/越界由返回码明确区分。

---

## v3.7 主题A：语义纯函数与质量感知写入口

> v3.7 新增（主题A：死区/RBE + EURange 生效）。均为**纯函数 / 薄封装**，无状态、不触共享内存，集中收口"值→语义"逻辑。

### 元数据 flags 位（indurtdb_meta_t.flags）

| 宏 | 位 | 含义 |
|---|---|---|
| `INDURTDB_META_FLAG_EUR` | bit0 | 启用量程检查（EURange → 量程位） |
| `INDURTDB_META_FLAG_DEADBAND` | bit1 | 启用绝对死区（阈值 = `deadband`） |
| `INDURTDB_META_FLAG_DEADBAND_PCT` | bit2 | 死区为百分比模式（阈值 = `deadband% × (eur_max − eur_min)`） |

> 仅新增 bit2，结构体布局不变（仍 32B）。

### indurtdb_value_to_double / indurtdb_eurange_limit / indurtdb_deadband_exceeded

```c
double  indurtdb_value_to_double(uint8_t type, const void* value_bits);
uint8_t indurtdb_eurange_limit(bool eur_enabled, double eur_min, double eur_max, double value);
bool    indurtdb_deadband_exceeded(bool abs_mode, double deadband,
                                   double eur_min, double eur_max,
                                   double last, double cur);
```

- `indurtdb_value_to_double`：6 种数值类型 → `double` 归一化（按 `type` 解析 `value_bits`，与 `indurtdb_point_t.value` union 一致）；供死区/RBE 与 EURange 比较。`string` 不适用，调用方应仅对数值类型使用。
- `indurtdb_eurange_limit`：`eur` 未启用返回 `INDURTDB_LIMIT_NONE`；`value < eur_min → INDURTDB_LIMIT_LOW`，`> eur_max → INDURTDB_LIMIT_HIGH`，区间内（含边界）→ `NONE`。
- `indurtdb_deadband_exceeded`：`abs_mode`=绝对阈值；否则（百分比）阈值 = `deadband% × (eur_max − eur_min)`。严格大于阈值才触发（等号不触发）；百分比模式 `eur_max == eur_min` 时不触发（除零保护）。

### indurtdb_write_quality_ts / indurtdb_h_write_quality_ts

```c
int indurtdb_write_quality_ts(uint32_t id, uint8_t type, const void* value,
                              uint64_t source_ts_ns, uint8_t quality);
int indurtdb_h_write_quality_ts(indurtdb_t* h, uint32_t id, uint8_t type, const void* value,
                                uint64_t source_ts_ns, uint8_t quality);
```

**质量感知写入口**（v3.7 主题A）。`value` 按 `type` 解释（与 `indurtdb_value_to_double` 一致），一次性写入值 + 调用方计算好的 `quality`（含 EURange 量程位，用 `INDURTDB_QUALITY_MAKE` 组装）。

- **默认 `indurtdb_write_*` 行为完全不变**：核心热路径仍强制 `quality = GOOD`、`零 meta 读`（守住 v3.4 T3「meta 不进热路径」不变式）。
- 本入口专供**写权威 `rtdbd`** 使用：在 `do_write` 读取该点 meta 计算量程位并经此落值；直接 shm 写者（legacy 路径）默认不含量程位，与「rtdbd 是写权威」一致。
- 典型用法：`rtdbd` 收到写请求 → `indurtdb_get_meta` → `indurtdb_value_to_double` → `indurtdb_eurange_limit`（算量程位）→ `INDURTDB_QUALITY_MAKE(GOOD, lim)` → `indurtdb_write_quality_ts(...)`。

### rtdbd 通知门控语义（RBE，v3.7 主题A）

`rtdbd` 对每个订阅连接按 `rbe_decide()` 决定是否推送 `NOTIFY`——**仅当死区越过或 EURange 边界跨越才推送**，区间内常规变化不推送（值仍已落库、GET 轮询可见）。

- 仅开启 `INDURTDB_META_FLAG_EUR`（无死区）时同样适用：推送**仅在跨量程边界时触发**，区间内变化不推送。这是「按订阅者 Reporting-By-Exception」的预期行为，非缺陷。
- 默认 `flags=0`：每次写都通知（零行为变更）。

---

## v3.7 主题B：运维硬化（健康 / 配置校验 / 脱离保留段）

> v3.7 新增（主题B：运维硬化）。库侧为**纯函数或薄封装**；rtdbd 侧的背压/健康/退出码见文末。

### indurtdb_self_check / indurtdb_h_self_check（B1）

```c
/* 健康状态枚举 */
#define INDURTDB_HEALTH_OK        0
#define INDURTDB_HEALTH_DEGRADED  1
#define INDURTDB_HEALTH_UNHEALTHY 2

int indurtdb_h_self_check(indurtdb_t* h);
int indurtdb_self_check(void);   /* v1 薄封装（默认句柄） */
```

**纯读**校验共享内存段头：`magic == IRT_MAGIC`、`version == IRT_SHM_VERSION`，以及段存在且
已初始化。不改写任何状态，可在 poll 循环 / 信号 dump 路径安全调用。

- 返回 `INDURTDB_HEALTH_OK` 或 `INDURTDB_HEALTH_UNHEALTHY`。
- **`DEGRADED` 由调用方分层判定**，核心不自行发出：例如 `rtdbd` 依据错误计数降级。
- 失败详情见 `indurtdb_get_last_error()`。

### indurtdb_validate_point_meta（B3，纯函数）

```c
int indurtdb_validate_point_meta(const indurtdb_meta_t* m, uint32_t* err_field);
```

校验单点元数据语义（**不触共享内存**，可单测）。合法返回 `INDURTDB_CFG_OK`；
否则返回错误码并把字段标识写入 `*err_field`（可为 `NULL`）。

| 错误码 | 含义 |
|---|---|
| `INDURTDB_CFG_OK` | 合法 |
| `INDURTDB_CFG_ERR_NULL` | meta 指针为空 |
| `INDURTDB_CFG_ERR_EUR_RANGE` | 启用量程但 `eur_min >= eur_max` |
| `INDURTDB_CFG_ERR_DEADBAND` | `deadband` 为负 |
| `INDURTDB_CFG_ERR_DEADBAND_PCT` | 百分比死区不在 `[0,100]` |
| `INDURTDB_CFG_ERR_FLAGS` | `flags` 含未知位 |
| `INDURTDB_CFG_ERR_TYPE` | 点位 type 越界 |
| `INDURTDB_CFG_ERR_NAME` | 点位名为空 |
| `INDURTDB_CFG_ERR_ACCESS` | access 既非只读也非读写 |

**刻意不强制**：百分比死区与 `INDURTDB_META_FLAG_EUR` **正交**——百分比死区只是**借用**
`eur_min`/`eur_max` 作跨度基准，与"是否在 quality 上置量程位"无关，故不要求二者同时启用。

**写入侧同样校验**：`indurtdb_h_set_meta()` 在落盘前调用本函数，非法 meta 一律拒写
（返回 `INDURTDB_ERR_ARG`），从源头杜绝"非法 meta 落盘 → 下次启动 fail-fast"。

### indurtdb_meta_pct_without_range（B3，便捷谓词）

```c
bool indurtdb_meta_pct_without_range(const indurtdb_meta_t* m);
```

置了百分比死区但量程跨度无效（`eur_max <= eur_min`）。此时阈值为 0，**死区永不触发**——
属可观测的静默失效，**非致命配置错误**。`rtdbd` 启动时对此打 WARN 而非拒绝。

### indurtdb_validate_config（B3）

```c
int indurtdb_validate_config(uint32_t* bad_id, uint32_t* err_field,
                             const char** err_reason);
```

遍历默认实例已注册点位，逐点校验（`type`/`access` 合法 + 元数据语义）。

- 返回 `INDURTDB_CFG_OK` 表示全部合法；否则返回错误码，并把首个非法点 id 写入 `*bad_id`。
- 三个输出参数均可为 `NULL`。
- **契约为何用返回值区分而非用点 id**：点 id 0 是合法点位，若以返回值 `0` 表示"全部合法"，
  则 id 为 0 的非法点会被误判为配置合法、绕过 fail-fast。

### indurtdb_cfg_error_reason

```c
const char* indurtdb_cfg_error_reason(int err_code);
```

错误码 → 人类可读原因文本（静态字符串，零分配）。

### rtdbd 运维语义（主题B）

**背压（B2）**：每连接定长 64 槽出站环形队列（零堆）。写成功只入队，队列满则**丢最旧（保序）**
并计 `notify_drop`；主循环仅在有待发时监听 `POLLOUT`。**慢消费者不再拖垮服务端**（消除
head-of-line 阻塞）。`outq_flush` 用 `MSG_DONTWAIT` + 发送进度做**部分发送**——`POLLOUT` 就绪
只保证缓冲有*部分*空间，阻塞 `send_all` 会在嵌入式小缓冲下卡死整个单线程服务端。

**通知为异步投递**：写响应后通知在下一轮 `POLLOUT` 送达（缓冲有空间时亚毫秒级），
不再与写响应同轮同步送达。

**健康（B1）**：`RTDBD_OP_HEALTH`（opcode 16）**只读、免鉴权**（负载仅计数与连接数，
不含点位值）；**协议主版本仍为 2**。SIGUSR1 触发 `--stats-file` dump
（信号处理器只置标志，dump 在主循环）。`DEGRADED` 按 **60s 滑动窗口**判定，可自愈。

**退出码（B2/B3）**

| 码 | 含义 | 场景 |
|---|---|---|
| 0 | 正常退出 | — |
| 1 | 通用 / 启动错误 | 参数解析、policy 加载失败等 |
| 2 | 配置校验失败 | `load_config` 失败、点位语义非法 |
| 3 | 共享内存损坏 / 初始化失败 | `indurtdb_initialize` 失败 |
| 4 | 启动自检失败 | 段头 `magic` / `version` 不通过 |

supervisor 对**致命码 2 / 4 停止 respawn** 并透传退出码，避免坏配置陷入重启风暴。
systemd 部署需配套 `RestartPreventExitStatus=2 4`（否则重启风暴只是转移到 systemd 层面）。

**fail-fast 保留数据**：配置或自检失败退出时用 `indurtdb_detach()` **保留共享内存段**，
不因配置写错销毁已有点位与值。未给 `--config` 时段内残留的非法 meta **只告警不致命**
（避免 v3.6 时代部署被砖化）。

---

### indurtdb_check_timeouts

```c
int indurtdb_check_timeouts(uint64_t timeout_ns);
```

扫描全部点位, 将超过 `timeout_ns` 未更新的点位标记为 `QUALITY_TIMEOUT`, 返回本次检测到的超时点数。`timeout_ns = 0` 时不做任何处理。

**约束**: 该扫描为 **best-effort** —— 遍历过程中若某个点位正处在写冲突状态会跳过该点, 因此单次调用不保证覆盖全部点位; 建议周期性调用使其收敛。时钟获取失败时整体跳过, 不会把所有点误标为 TIMEOUT。

**严苛边界(实测)**: 若写入者以最大速率**自旋**写入(无间隙连续 `write_*`), 全局单 seqlock 的"偶数窗口"极短, 扫描可能**整轮取不到写锁而返回 0**。真实采集周期(ms 级)下不会触发。该边界已立 [issue #19](https://github.com/turnarond/InduRTDB/issues/19) 供后续评估(扫描侧让步/退避、写锁有限重试等)。

### indurtdb_get_write_count

```c
uint64_t indurtdb_get_write_count(void);
```

返回全局写入总次数 (从共享内存 stats 读取)。

### indurtdb_get_timeout_count

```c
uint64_t indurtdb_get_timeout_count(void);
```

返回超时点位计数 (从共享内存 stats 读取)。

### indurtdb_get_scan_skipped（v3.4 T7）

```c
uint64_t indurtdb_get_scan_skipped(void);
```

返回超时扫描因写锁冲突被**跳过**的点数累计（可观测「饿死」）。Header 该字段位于保留区，**不计入 CRC**，运行时自增不会使 attach 校验失败。

---

## 质量 OPC UA 映射（v3.4 新增）

库内置 `quality` ↔ OPC UA `StatusCode` 的双向纯函数映射（无状态、零依赖），便于北向上送时保留工业语义。

| 函数 | 说明 |
|------|------|
| `indurtdb_quality_to_status_code(uint8_t quality)` | 点位质量 → OPC UA `StatusCode`（uint32） |
| `indurtdb_status_code_to_quality(uint32_t status_code)` | OPC UA `StatusCode` → 点位质量；未知码回退 `BAD` |
| `indurtdb_quality_is_usable(uint8_t quality)` | 「值是否可用」只看基础码（量程位正交），返回 1/0 |

**映射约定**：base 码决定 severity（bit30-31）+ 子状态（code, bit0-15）；**量程位映射到 StatusCode 保留位（bit28-29）**（远离 severity、避开 OPC UA 已定义位 bit24/25，仅用于本库产物回读，不得原样投递第三方 OPC UA 栈），与 severity/code 正交。`write_*` 将 quality 置为纯 `GOOD`（量程位清零）；如需量程位，应在写之后调用 `indurtdb_set_quality`。`check_timeouts` 超时刻**保留**量程位（基础码改为 `TIMEOUT`，量程位不丢）。

---

## 错误处理

### indurtdb_get_last_error

```c
const char* indurtdb_get_last_error(void);
```

返回最近一次错误的描述字符串。

常见错误:
| 错误 | 说明 |
|------|------|
| `already initialized` | 重复调用 `initialize()` |
| `invalid argument` | instance_id 为空或 max_points 为 0 |
| `shm init failed` | 共享内存创建/附加失败 (magic/version 不匹配或权限不足) |

---

## indurtdb-client（受控通道 API，v3.3）

`indurtdb-client` 与核心库**并列但独立**：核心库不感知 RPC，容器进程或需要受控写入的进程使用本客户端。
头文件 `client/include/irtcli/client.h`，链接目标 `irtcli`。

| 函数 | 说明 |
|---|---|
| `irtcli_init(c, sock_path, cap, alert, user_data)` | 初始化；`cap = 0` 用默认 256。返回 `IRTCLI_OK` 或负错误码 |
| `irtcli_close(c)` | 释放队列与连接 |
| `irtcli_connect(c)` | 建立 / 重连 `rtdbd`；失败时仍可异步入队 |
| `irtcli_set_async(c, bool)` | 切换异步（默认）/ 同步模式 |
| `irtcli_write_bool / int32 / double(c, id, value, source_ts_ns)` | 写入；异步返回 `IRTCLI_QUEUED`，同步返回 `IRTCLI_OK` |
| `irtcli_flush(c)` | 按序提交队列；返回提交条数。IO 失败会**先重连再重试一次**，连接不可用则保留队列（不丢） |
| `irtcli_queue_count(c)` | 当前队列长度 |

**返回码**：`IRTCLI_OK` 0（已提交并确认）、`IRTCLI_QUEUED` 1（已入队未提交）、`ERR_ARG` -1、`ERR_FULL` -2（队列满，同时触发告警）、`ERR_IO` -3、`ERR_DENIED` -4（鉴权拒绝）、`ERR_PROTO` -5。

**语义要点**：

- 同点位合并去重（只保留最新值）；队列满触发告警回调，**绝不静默丢弃**；
- 重放携带**原始** `source_ts_ns`，采集时刻不失真；
- `send()` 使用 `MSG_NOSIGNAL`：`rtdbd` 崩溃只返回错误码，**不会以 `SIGPIPE` 终止调用进程**。

## 完整示例

```c
#include <indurtdb/indurtdb.h>
#include <stdio.h>

int main() {
    // 初始化 (首个进程创建共享内存, 后续进程附加)
    if (indurtdb_initialize("hvac_system", 10000, 32) != 0) {
        fprintf(stderr, "init failed: %s\n", indurtdb_get_last_error());
        return 1;
    }

    // 写入
    indurtdb_write_double(1001, 23.5);
    indurtdb_write_int32(2001, 42);
    indurtdb_write_bool(3001, true);
    indurtdb_write_string(4001, "Running");

    // 读取
    double temp;
    if (indurtdb_read_double(1001, &temp) == 0) {
        printf("温度: %.1f\n", temp);
    }

    // 完整点位读取
    indurtdb_point_t pt;
    if (indurtdb_read_point(1001, &pt) == 0) {
        printf("type=%d quality=%d timestamp=%lu\n",
               pt.type, pt.quality, (unsigned long)pt.timestamp_ns);
    }

    // peek 快速读取 (高频场景)
    const indurtdb_point_t* p = indurtdb_peek(1001);
    if (p) printf("温度(peek): %.1f\n", p->value.d);

    // 清理
    indurtdb_shutdown();
    return 0;
}
```

## 跨语言调用

### Python (ctypes)

```python
import ctypes

lib = ctypes.CDLL("libindurtdb.so")

class PointData(ctypes.Structure):
    _fields_ = [
        ("value_b", ctypes.c_bool),
        ("_pad1", ctypes.c_uint8 * 3),
        ("value_i", ctypes.c_int32),
        ("value_d", ctypes.c_double),
        ("value_str", ctypes.c_char * 32),
        ("timestamp_ns", ctypes.c_uint64),
        ("type", ctypes.c_uint8),
        ("quality", ctypes.c_uint8),
        ("unit", ctypes.c_uint16),
        ("access", ctypes.c_uint8),
        ("name", ctypes.c_char * 64),
        ("padding", ctypes.c_uint8 * 19),
    ]

lib.indurtdb_initialize(b"my_app", 1000, 32)
lib.indurtdb_write_double(1, ctypes.c_double(25.0))

val = ctypes.c_double()
lib.indurtdb_read_double(1, ctypes.byref(val))
print(f"温度: {val.value}")

lib.indurtdb_shutdown()
```

### Rust (FFI)

```rust
extern "C" {
    fn indurtdb_initialize(instance_id: *const c_char,
                           max_points: u32,
                           max_subscribers: u32) -> i32;
    fn indurtdb_write_double(id: u32, value: f64) -> i32;
    fn indurtdb_read_double(id: u32, value: *mut f64) -> i32;
    fn indurtdb_peek(id: u32) -> *const IndurtdbPoint;
    fn indurtdb_shutdown();
}
```
