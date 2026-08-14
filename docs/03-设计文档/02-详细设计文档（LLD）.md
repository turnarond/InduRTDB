# InduRTDB 详细设计文档（LLD）

**版本：3.1.0**
**日期：2026年8月15日**
**修订说明**：v3.1.0 新增崩溃自愈（header 填充区 owner_pid 字段 + irt_shm_os_claim_ownership 接管 + 奇数 write_seq 恢复）。
（v3.0.0：纯 C11 重写——所有组件从 C++ 类改为 C 结构体 + 自由函数；使用实际实现中的真实函数签名和文件名 (irt_*.c/h)）

---

## 1. 共享内存精确布局

### 1.1 内存段命名与创建

- **段名格式**：`/indurtdb_<instance_id>`（如 `/indurtdb_hvac`）
- **创建者**：首个调用 `InduRTDB::instance().initialize()` 的进程
- **权限**：`0666`

### 1.2 内存布局（按偏移对齐）

| 偏移 (Byte) | 字段 | 类型 | 说明 |
|-------------|------|------|------|
| 0 | `magic` | `uint32_t` | 固定值 `0x1DBA1DBA` |
| 4 | `version` | `uint32_t` | 当前为 `1` |
| 8 | `max_points` | `uint32_t` | 最大点位数（默认 10000） |
| 12 | `max_subscribers` | `uint32_t` | 最大订阅者数（默认 32） |
| 16 | `write_seq` | `uint64_t` | **全局** Seqlock 序列号（偶=空闲，奇=写中） |
| 24 | `stats.writes` | `uint64_t` | 总写入次数 |
| 32 | `stats.timeouts` | `uint64_t` | 超时点位计数 |
| 40–63 | **Padding** | — | 对齐到 64B |

> ✅ **Header 大小 = 64 字节**（`sizeof(InduRTDBHeader) == 64`）

### 1.3 PointData 结构（定长 128 字节）

```cpp
#pragma pack(push, 1)
struct PointData {
    union Value {
        bool      b;            // 1B
        int32_t   i;            // 4B
        double    d;            // 8B
        char      str[32];      // 32B（仅状态文本）
    } value;                    // offset=0, size=32

    uint64_t  timestamp_ns;     // offset=32, size=8
    uint8_t   type;             // offset=40, size=1
    uint8_t   quality;          // offset=41, size=1
    uint16_t  unit;             // offset=42, size=2
    uint8_t   access;           // offset=44, size=1
    char      name[64];         // offset=45, size=64

    uint8_t   padding[19];      // offset=109, size=19 → align to 128B
};
#pragma pack(pop)

static_assert(sizeof(PointData) == 128, "PointData must be 128 bytes");
```

- **点位数组起始偏移**：`64`
- **点位 i 地址**：`base + 64 + i * 128`

### 1.4 SubscriberTable（订阅者表）

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| `64 + MAX_POINTS*128` | `pid` | `pid_t`（4B） | 进程 ID |
| +4 | `last_heartbeat_ns` | `uint64_t` | 最后心跳时间（单调纳秒） |
| +12 | **Padding** | 4B | 对齐到 16B |

- **每条目大小 = 16 字节**
- **表大小 = `MAX_SUBSCRIBERS * 16`**

### 1.5 总内存大小计算

```cpp
total_size = sizeof(InduRTDBHeader)                     // 64
           + max_points    * sizeof(PointData)          // N × 128
           + max_subscribers * sizeof(SubscriberEntry); // M × 16
```

---

## 2. 核心模块设计（纯 C11 实现）

所有模块使用 C 结构体 + 自由函数，无虚函数，无 STL，零堆分配。

### 2.1 点位管理器 (`irt_point_manager.c/h`)

```c
// irt_point_manager.h
typedef struct {
    irt_header_t*  header;       // → 共享内存头部 (write_seq, stats)
    irt_point_t*   points;       // → 共享内存点位数组
    uint32_t       max_points;
} irt_pm_t;

// 初始化: 从共享内存基址计算 header/points 偏移
void irt_pm_init(irt_pm_t* pm, void* shm_base, uint32_t max_points);

// 4 个显式类型写入函数（替代 C++ template<T>）
int irt_pm_write_bool(irt_pm_t* pm, uint32_t id, bool value);
int irt_pm_write_int32(irt_pm_t* pm, uint32_t id, int32_t value);
int irt_pm_write_double(irt_pm_t* pm, uint32_t id, double value);
int irt_pm_write_string(irt_pm_t* pm, uint32_t id, const char* value);

// 4 个显式类型读取函数
int irt_pm_read_bool(const irt_pm_t* pm, uint32_t id, bool* out);
int irt_pm_read_int32(const irt_pm_t* pm, uint32_t id, int32_t* out);
int irt_pm_read_double(const irt_pm_t* pm, uint32_t id, double* out);
int irt_pm_read_string(const irt_pm_t* pm, uint32_t id, char* buf, size_t sz);
int irt_pm_read_point(const irt_pm_t* pm, uint32_t id, irt_point_t* out);

// 零拷贝 peek，内部使用 Seqlock 重试保护
const irt_point_t* irt_pm_peek(const irt_pm_t* pm, uint32_t id);

// 批量读写
int irt_pm_read_range(const irt_pm_t* pm, uint32_t start, uint16_t count,
                      irt_point_t* out, uint16_t out_cap);
int irt_pm_write_range_bool(irt_pm_t* pm, uint32_t start,
                            const bool* values, uint16_t count);
int irt_pm_write_range_int32(irt_pm_t* pm, uint32_t start,
                             const int32_t* values, uint16_t count);
int irt_pm_write_range_double(irt_pm_t* pm, uint32_t start,
                              const double* values, uint16_t count);

// 统计查询
uint64_t irt_pm_get_write_count(const irt_pm_t* pm);
uint64_t irt_pm_get_timeout_count(const irt_pm_t* pm);
```

**关键设计决策**：
- **显式类型函数**：C 无模板，用 4 个类型化函数替代 `write<T>()`
- **Seqlock 写入流程**：`irt_seqlock_write_begin()` → 写数据 → `irt_seqlock_write_end()`，冲突返回 `IRT_ERR_BUSY`
- **`peek()` 在 Seqlock 重试循环内直接返回 `&points[id]`**：真正零拷贝

### 2.2 Seqlock (`irt_seqlock.h`, inline 实现)

```c
// irt_seqlock.h — 全部 inline 自由函数，操作 header->write_seq
#define IRT_SEQLOCK_RETRY_MAX 3

static inline uint64_t irt_seqlock_write_begin(uint64_t* seq) {
    uint64_t s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s0 & 1ULL) return 0;  // 写冲突
    __atomic_store_n(seq, s0 + 1, __ATOMIC_RELEASE);
    return s0;
}

static inline void irt_seqlock_write_end(uint64_t* seq, uint64_t s0) {
    __atomic_store_n(seq, s0 + 2, __ATOMIC_RELEASE);
}

static inline int irt_seqlock_read_begin(const uint64_t* seq, uint64_t* out) {
    uint64_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s & 1ULL) return 0;  // 正在写
    *out = s;
    return 1;
}

static inline int irt_seqlock_read_retry(const uint64_t* seq, uint64_t s0) {
    return __atomic_load_n(seq, __ATOMIC_ACQUIRE) == s0;
}
```

### 2.3 订阅管理器 (`irt_subscription.c/h`)

```c
// irt_subscription.h
#define IRT_SUB_MAX_CALLBACKS 256

typedef void (*irt_sub_callback_t)(uint32_t id,
    const irt_point_t* data, void* user_data);

typedef struct {
    uint32_t           point_id;
    irt_sub_callback_t callback;
    void*              user_data;
    uint64_t           last_heartbeat_ns;
    bool               active;
} irt_sub_slot_t;

typedef struct {
    irt_sub_slot_t     slots[IRT_SUB_MAX_CALLBACKS];  // 定长数组
    uint32_t           slot_count;
    irt_subscriber_t*  shm_table;     // → 共享内存心跳表
    uint32_t           max_subscribers;
} irt_sub_t;

void irt_sub_init(irt_sub_t* sub, irt_subscriber_t* shm_table,
                  uint32_t max_subscribers);
int  irt_sub_register(irt_sub_t* sub, uint32_t id,
                      irt_sub_callback_t cb, void* user_data);
int  irt_sub_unregister(irt_sub_t* sub, uint32_t id);
void irt_sub_notify(irt_sub_t* sub, uint32_t id, const irt_point_t* data);
void irt_sub_update_heartbeat(irt_sub_t* sub, pid_t pid);
void irt_sub_cleanup_zombies(irt_sub_t* sub, uint64_t timeout_ns);
```

### 2.4 共享内存段 (`irt_shm.c/h`)

```c
// irt_shm.h
typedef struct {
    char              name[64];      // "/indurtdb_<instance_id>"
    void*             base;          // mmap 返回基址
    size_t            total_size;
    bool              is_owner;
    irt_header_t*     header;         // 偏移 0
    irt_point_t*      points;         // 偏移 sizeof(irt_header_t)
    irt_subscriber_t* subscribers;    // 偏移 points 之后
    uint32_t          max_points;
    uint32_t          max_subscribers;
} irt_shm_t;

int  irt_shm_init(irt_shm_t* shm, const char* instance_id,
                  uint32_t max_points, uint32_t max_subscribers);
void irt_shm_shutdown(irt_shm_t* shm);
```

### 2.5 配置加载器 (`irt_config.c/h`)

```c
// irt_config.h — 轻量 key=value 解析器，零 malloc
int  irt_config_load(const char* path,
                     uint32_t* max_points, uint32_t* max_subscribers);
int  irt_config_parse_line(const char* line, char* key, size_t key_sz,
                           char* value, size_t val_sz);
```

---

## 3. 错误处理策略

| 错误场景 | 处理方式 | 返回值 |
|---------|--------|--------|
| `write(id)` 中 id 越界 | 返回 false | `false` |
| Seqlock 写冲突 | 返回 false（调用方可重试） | `false` |
| shm_open 失败 | 日志 FATAL + 返回 false | `false` |
| magic 不匹配 | 日志 FATAL，abort() | `abort()` |
| 配置解析失败 | 日志 ERROR | `false` |

**无异常**：全部使用 `bool` 返回值，错误详情通过 `utils::set_error()` 记录。

---

## 4. 文件组织（v3.0.0 纯 C11）

```
indurtdb/
├── include/indurtdb/
│   └── indurtdb.h              # 唯一公共头文件 (纯 C API, 26 函数)
├── src/
│   ├── api/
│   │   └── indurtdb.c           # 公共 API 单例实现
│   ├── core/
│   │   ├── irt_shm.c/h          # 共享内存段管理
│   │   ├── irt_point_manager.c/h # 点位读写 (Seqlock 保护)
│   │   ├── irt_subscription.c/h  # 订阅/通知/心跳
│   │   └── irt_config.c/h        # key=value 配置解析
│   ├── internal/
│   │   ├── irt_types.h           # 共享内存布局定义
│   │   └── irt_seqlock.h         # Seqlock inline 函数
│   └── osal/
│       ├── irt_osal.h            # OS 抽象接口 (纯 C)
│       ├── posix/irt_osal_posix.c
│       └── sylixos/irt_osal_sylixos.c
├── tests/
│   ├── unit/
│   │   ├── test_c_api.cpp        # 公共 API 全功能验证
│   │   ├── test_c_config.cpp     # 配置加载器
│   │   ├── test_c_layout_seqlock.cpp # 布局 + Seqlock
│   │   ├── test_c_osal.cpp       # OSAL 层
│   │   ├── test_c_pm.cpp         # 点位管理器
│   │   ├── test_c_shm.cpp        # 共享内存段
│   │   └── test_c_sub.cpp        # 订阅管理器
│   └── integration/
│       └── test_c_multi_process.cpp # 多进程 fork + 布局回归
├── examples/
│   ├── basic_example.c           # C11 使用示例
│   └── CMakeLists.txt
├── CMakeLists.txt
└── VERSION
```

---

## 5. 版本演变关键变更

| 项目 | v1.0 (C++) | v2.0 (C+重构) | v3.0 (纯 C11) |
|------|-----------|--------------|---------------|
| 语言 | C++17 | C++17 | **C11 (gnu11)** |
| Seqlock | ISeqlock 类层次 | 3 个自由函数 | `irt_seqlock.h` 4 个 inline 函数 |
| PointManager | 虚接口 + `vector<unique_ptr<ISeqlock>>` | 非虚类 + 直接操作共享内存 | `irt_pm_t` 结构体 + 自由函数 |
| SubscriptionManager | `unordered_map` + `vector` + `function` | 定长 `SubscriberSlot[256]` + 函数指针 | `irt_sub_t` + `irt_sub_callback_t` |
| OSAL | 虚接口 (ISharedMemory 等) | 同左 | `irt_osal.h` 纯 C 函数 (4 个导出) |
| API | C++ 类 + C ABI 桥接 | C++ 单例 + C ABI 17 函数 | `indurtdb.h` 单一头文件 26 个 C 函数 |
| 公共头 | `indurtdb.hpp` + `c/indurtdb_c.h` | 同左 | **仅** `indurtdb/indurtdb.h` |
| 编译选项 | `-std=c++11` | `-std=c++11` | `-std=gnu11 -Wall -Wextra -Werror` |
| 测试 | C++ gtest | C++ gtest | C++17 gtest (仅测试)，8 suites |

---

**文档变更记录**
- v1.0.0 (2026-03-26)：初始版本
- v2.0.0 (2026-05-11)：移除 STL 依赖，Seqlock 回归全局机制，PointManager 非虚化
- v3.0.0 (2026-07-21)：纯 C11 重写——全部 C++ 类替换为 C 结构体+自由函数；使用实际 `irt_*.c/h` 文件名和函数签名；单一公共头 `indurtdb.h` (26 函数)
