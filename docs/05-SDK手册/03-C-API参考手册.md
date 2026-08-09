# InduRTDB C API 参考手册

**版本**: 3.1.0 | **更新日期**: 2026-08-09 | **变更**: 对齐 v3.1.0 (26 函数 API 不变)

**头文件**: `<indurtdb/indurtdb.h>`
**链接**: `-lindurtdb -lpthread -lrt`

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

### indurtdb_read_point

```c
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
```

通过 Seqlock 协议读取完整点位数据 (128 字节安全拷贝)。

### indurtdb_peek (零拷贝)

```c
const indurtdb_point_t* indurtdb_peek(uint32_t id);
```

返回共享内存中点位数据的**直接指针** (零拷贝)。ID 无效时返回 NULL。

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

**约束**: 最多 256 个订阅槽位。回调中不得执行耗时操作或嵌套写入。

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

    // 零拷贝 peek (高频场景)
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
