# InduRTDB C++ API 参考手册

**版本**: 3.0.0 | **更新日期**: 2026-07-21

> **重要声明**: v3.0.0 起, InduRTDB 已完全重写为纯 C11 实现。C++ API (v2.x `InduRTDB` 类) 已移除。
> 本手册保留作为 v2.x 用户迁移参考。
>
> **推荐使用 C API**: 请查阅 [C API 参考手册](./C%20API%20%E5%8F%82%E8%80%83%E6%89%8B%E5%86%8C.md) (v3.0.0)。
> C++ 项目可通过 `extern "C" { #include <indurtdb/indurtdb.h> }` 直接调用。
>
> **迁移要点**:
> | v2.x (C++) | v3.0.0 (C) |
> |---|---|
> | `InduRTDB::instance()` | 隐式单例 (全局 static) |
> | `rtdb.write(id, 23.5)` | `indurtdb_write_double(id, 23.5)` |
> | `rtdb.write(id, (int32_t)42)` | `indurtdb_write_int32(id, 42)` |
> | `rtdb.read(id, p)` | `indurtdb_read_point(id, &p)` |
> | `rtdb.peek(id)` | `indurtdb_peek(id)` |
> | `<indurtdb.hpp>` | `<indurtdb/indurtdb.h>` |

---

以下为 v2.x C++ API 原始文档 (仅作参考):

---

## 命名空间

所有类型和函数位于 `indurtdb` 命名空间。唯一公开头文件：`<indurtdb.hpp>`。

## 主类：`InduRTDB`

单例类（PIMPL 模式），管理整个实时数据库的生命周期。

### 获取实例

```cpp
static InduRTDB& InduRTDB::instance();
```

**返回值**：全局唯一实例的引用。线程安全（C++11 static 局部变量）。

**示例**：
```cpp
auto& rtdb = indurtdb::InduRTDB::instance();
```

---

### 初始化

```cpp
bool initialize(const char* instance_id,
                uint32_t max_points = 10000,
                uint32_t max_subscribers = 32);
```

**参数**：
| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `instance_id` | `const char*` | — | 实例名，用于生成共享内存段名 `/indurtdb_<id>` |
| `max_points` | `uint32_t` | 10000 | 最大点位数，决定共享内存大小 |
| `max_subscribers` | `uint32_t` | 32 | 最大订阅者进程数 |

**返回值**：`true` 成功，`false` 失败（instance_id 为空、共享内存创建失败、magic 校验失败）。

**说明**：
- 首个调用的进程成为 owner，创建并初始化共享内存段（magic = `0x1DBA1DBA`）
- 后续进程通过相同的 `instance_id` 附加到已有段，校验 magic 和 version
- 调用前确保没有残留的同名共享内存段（`/dev/shm/indurtdb_<id>`）

---

### 写入：`write<T>()`

```cpp
template<typename T>
bool write(PointId id, const T& value);
```

**支持类型**：
| T | 说明 | PointType |
|---|------|-----------|
| `bool` | 布尔值（开关、启停） | `BOOL (0)` |
| `int32_t` | 32 位有符号整数（计数器、档位） | `INT32 (1)` |
| `double` | 双精度浮点数（温度、压力） | `DOUBLE (2)` |

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` (uint32_t) | 点位 ID，0 ≤ id < max_points |
| `value` | `const T&` | 要写入的值 |

**返回值**：`true` 成功，`false` 失败（id 越界或 Seqlock 写冲突）。

**副作用**：自动更新 `timestamp_ns`（当前单调时间）和 `quality = GOOD`。递增 `stats.writes` 计数器。写入后自动 `notify()` 触发该点位的回调。

**示例**：
```cpp
rtdb.write(1001, 23.5);           // 写入温度
rtdb.write(2001, (int32_t)42);    // 写入计数器（注意显式转换）
rtdb.write(3001, true);           // 写入开关状态
```

### 写入字符串：`write(PointId, const char*)`

```cpp
bool write(PointId id, const char* value);
```

**非模板重载**，避免 `const char[N]` 字面量类型推导问题。

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |
| `value` | `const char*` | C 字符串，最大 31 字符（自动截断） |

---

### 拷贝读取：`read()`

```cpp
bool read(PointId id, PointData& out) const;
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |
| `out` | `PointData&` | 输出参数，接收完整点位数据副本（128 字节拷贝） |

**返回值**：`true` 成功，`false` 失败（id 越界）。

**说明**：通过 Seqlock 协议保证读到一致的数据（不会读到"写到一半"的撕裂数据）。

---

### 零拷贝读取：`peek()`

```cpp
const PointData* peek(PointId id) const;
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |

**返回值**：指向共享内存中 `PointData` 的 `const` 指针，失败返回 `nullptr`。

**关键限制**：
- 返回的指针直接指向共享内存，**不持有锁**
- 调用方不应长期持有此指针（跨越写入边界后数据可能变化）
- 适合高频读取场景（如控制循环中批量遍历点位）

**性能对比**：
| 方法 | 拷贝 (128B) | 预估延迟 (ARM A53) |
|------|------------|-------------------|
| `read()` | 有 | ~50ns |
| `peek()` | 无 | ~10ns |

---

### 订阅：`subscribe()`

```cpp
bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 要订阅的点位 ID |
| `cb` | `SubscriptionCallback` | 回调函数指针 |
| `user_data` | `void*` | 透传给回调的用户数据 |

**返回值**：`true` 成功，`false` 失败（id 越界、回调为 null、订阅槽位已满）。

**说明**：
- 全局最多 256 个订阅槽位
- 回调在写入者线程内**同步执行**，不要在回调中做耗时操作
- 回调签名见 `SubscriptionCallback` 类型定义

### 取消订阅：`unsubscribe()`

```cpp
bool unsubscribe(PointId id);
```

取消指定点位上的所有订阅。

---

### 配置加载：`load_config()`

```cpp
bool load_config(const char* config_path);
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `config_path` | `const char*` | YAML 配置文件路径 |

**返回值**：`true` 成功，`false` 失败（文件不存在、格式错误）。

**支持的 YAML 格式**：
```yaml
points:
  - id: 1001
    name: "AHU_01.Supply_Temp"
    type: double    # bool | int32 | double | string
    unit: 1         # NO_UNIT=0, °C=1, Pa=2, %=3
    access: 1       # READ_ONLY=1, READ_WRITE=3
```

---

### 心跳更新：`update_heartbeat()`

```cpp
void update_heartbeat();
```

更新当前进程的心跳时间戳。定期调用以避免被 `cleanup_zombies()` 清理。

---

### 关机：`shutdown()`

```cpp
void shutdown();
```

释放 PointManager、SubscriptionManager、SharedMemorySegment。若为 owner 进程，同时 `shm_unlink` 删除共享内存段。调用后 `is_initialized()` 返回 `false`。

---

### 状态查询：`is_initialized()`

```cpp
bool is_initialized() const;
```

**返回值**：`true` 已初始化且未关机。

---

### 统计查询：`get_write_count()`

```cpp
uint64_t get_write_count() const;
```

**返回值**：全局累计写入次数（从共享内存 stats 原子读取）。

---

## 数据类型参考

### PointData

```cpp
struct PointData {
    union Value {
        bool      b;            // 布尔值
        int32_t   i;            // 32 位整数
        double    d;            // 浮点数
        char      str[32];      // 字符串（最多 31 字符 + '\0'）
    } value;                    // 值联合体，根据 type 字段判断使用哪个成员

    uint64_t  timestamp_ns;     // 时间戳（CLOCK_MONOTONIC_RAW，纳秒）
    PointType type;             // 数据类型（BOOL=0, INT32=1, DOUBLE=2, STRING=3）
    Quality   quality;          // 数据质量（GOOD=0, BAD=1, TIMEOUT=2, SUBSTITUTED=3）
    Unit      unit;             // 单位（NO_UNIT=0, °C=1, Pa=2, %=3）
    Access    access;           // 访问权限（READ_ONLY=1, READ_WRITE=3）
    char      name[64];         // 点位名称（如 "AHU_01.Supply_Temp"）
};  // sizeof = 128 字节，aligned(128)
```

### 基础类型别名

```cpp
using PointId     = uint32_t;   // 点位标识符
using TimestampNs = uint64_t;   // 纳秒时间戳
using Pid         = int32_t;    // 进程 ID
```

### 枚举

```cpp
enum class PointType : uint8_t { BOOL = 0, INT32 = 1, DOUBLE = 2, STRING = 3 };
enum class Quality   : uint8_t { GOOD = 0, BAD = 1, TIMEOUT = 2, SUBSTITUTED = 3 };
enum class Access    : uint8_t { READ_ONLY = 1, READ_WRITE = 3 };
enum class Unit      : uint16_t { NO_UNIT = 0, DEGREES_CELSIUS = 1, PASCAL = 2, PERCENT = 3 };
```

### SubscriptionCallback

```cpp
using SubscriptionCallback = void (*)(PointId id,
                                      const PointData& data,
                                      void* user_data);
```

**参数**：
- `id` — 发生变化的点位 ID
- `data` — 点位数据引用（指向共享内存，回调内使用后即失效）
- `user_data` — `subscribe()` 时传入的用户数据指针

---

## 错误处理

- 所有 `bool` 返回值的函数，`false` 表示失败
- 错误详情通过日志输出（stderr）
- C API 额外提供 `indurtdb_get_last_error()` 获取最近错误消息

## 线程安全

- `write<T>()`：线程安全（Seqlock 保护），但设计为单写者模式，并发写会冲突返回 `false`
- `read()` / `peek()`：线程安全（Seqlock 保护），完全无锁
- `subscribe()` / `unsubscribe()`：非线程安全，应在初始化阶段完成
- `initialize()` / `shutdown()`：非线程安全，应在主线程单次调用

---

## 预留：异步接口 (规划中)

当前 subscribe 回调在写入者线程同步执行。未来将通过 Unix Domain Socket 实现跨进程异步通知：

```cpp
// 未来的异步订阅接口（规划中）
bool subscribe_async(PointId id, int notify_fd);
```

现有 `SubscriptionCallback` 签名兼容此扩展，无需变更。
