# InduRTDB C API 参考手册

**版本**：2.1.0

**头文件**：`<indurtdb/api/c/indurtdb_c.h>`
**链接**：`-lindurtdb -lpthread -lrt`

---

## 类型定义

### indurtdb_point_t

```c
typedef struct {
    union {
        bool b;
        int32_t i;
        double d;
        char str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t  type;       // 0=BOOL, 1=INT32, 2=DOUBLE, 3=STRING
    uint8_t  quality;    // 0=GOOD, 1=BAD, 2=TIMEOUT, 3=SUBSTITUTED
    uint16_t unit;       // NO_UNIT=0, °C=1, Pa=2, %=3
    uint8_t  access;     // 1=READ_ONLY, 3=READ_WRITE
    char name[64];
} indurtdb_point_t;
```

与 C++ `PointData` 布局兼容的 C 结构体。`sizeof` = 128 字节。

---

## 初始化与关闭

### indurtdb_initialize

```c
int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers);
```

**参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| `instance_id` | 实例名（段名 `/indurtdb_<id>`） | — |
| `max_points` | 最大点位数 | 10000 |
| `max_subscribers` | 最大订阅者数 | 32 |

**返回值**：0 成功，非 0 失败。

### indurtdb_shutdown

```c
void indurtdb_shutdown(void);
```

释放资源。owner 进程同时删除共享内存段。

---

## 写入函数

所有写入函数：成功返回 0，失败返回非 0（id 越界或写冲突）。

### indurtdb_write_bool

```c
int indurtdb_write_bool(uint32_t id, bool value);
```

写入布尔值。`type` 自动设为 `BOOL(0)`。

### indurtdb_write_int32

```c
int indurtdb_write_int32(uint32_t id, int32_t value);
```

写入 32 位有符号整数。`type` 自动设为 `INT32(1)`。

### indurtdb_write_double

```c
int indurtdb_write_double(uint32_t id, double value);
```

写入双精度浮点数。`type` 自动设为 `DOUBLE(2)`。

### indurtdb_write_string

```c
int indurtdb_write_string(uint32_t id, const char* value);
```

写入字符串（最多 31 字符，自动截断）。`type` 自动设为 `STRING(3)`。

---

## 读取函数

所有读取函数：成功返回 0，失败返回非 0（id 越界）。

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

**参数**：
| 参数 | 说明 |
|------|------|
| `buffer` | 用户提供的输出缓冲区 |
| `buffer_size` | 缓冲区大小（建议 ≥ 32 字节） |

### indurtdb_read_point

```c
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
```

读取完整点位数据（128 字节拷贝），通过 Seqlock 协议保证一致性。

---

## 验证与统计

### indurtdb_validate_id

```c
int indurtdb_validate_id(uint32_t id);
```

检查 id 是否在有效范围内。返回 0 表示有效。

### indurtdb_get_write_count

```c
uint64_t indurtdb_get_write_count(void);
```

返回全局写入总次数（从共享内存 stats 读取）。

### indurtdb_get_timeout_count

```c
uint64_t indurtdb_get_timeout_count(void);
```

返回超时点位计数。

---

## 错误处理

### indurtdb_get_last_error

```c
const char* indurtdb_get_last_error(void);
```

返回最近一次错误的描述字符串。线程本地存储。

---

## 跨语言调用

### Python (ctypes)

```python
import ctypes

lib = ctypes.CDLL("libindurtdb.so")

lib.indurtdb_initialize(b"my_app", 1000, 32)
lib.indurtdb_write_double(1, ctypes.c_double(25.0))

val = ctypes.c_double()
lib.indurtdb_read_double(1, ctypes.byref(val))
print(f"温度: {val.value}")

lib.indurtdb_shutdown()
```

### Rust (FFI)

使用 `bindgen` 从 `indurtdb_c.h` 自动生成绑定，或手动声明 extern 块：

```rust
extern "C" {
    fn indurtdb_initialize(instance_id: *const c_char, max_points: u32, max_subscribers: u32) -> i32;
    fn indurtdb_write_double(id: u32, value: f64) -> i32;
    fn indurtdb_read_double(id: u32, value: *mut f64) -> i32;
    fn indurtdb_shutdown();
}
```
