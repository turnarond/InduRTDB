# InduRTDB 详细设计文档（LLD）

**版本：2.1.0**
**日期：2026年5月16日**
**修订说明**：对齐 v2.1.0 代码实现，修正文件名引用和成员变量名

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

## 2. 核心类设计

### 2.1 PointManager（非虚，直接操作共享内存）

```cpp
// point_manager.hpp
class PointManager {
public:
    PointManager(void* shm_base, uint32_t max_points,
                 osal::ITime* time);

    // 模板写入（满足 SRS）
    template<typename T>
    bool write(PointId id, const T& value);

    // 零拷贝读取
    bool read(PointId id, PointData& out) const;
    const PointData* peek(PointId id) const;   // 直接返回 &points_[id]

    bool validate_id(PointId id) const;
    uint64_t get_write_count() const;
    uint64_t get_timeout_count() const;

private:
    InduRTDBHeader* header_;    // → 共享内存头部
    PointData*      points_;    // → 共享内存点位数组
    uint32_t        max_points_;
    osal::ITime*    time_;
};
```

**关键设计决策**：
- **非虚类**：不需要多态，直接编译期绑定
- **构造时接收 `void* shm_base`**：由 SharedMemorySegment 传入 mmap 地址
- **模板 `write<T>`**：编译期类型分发，替代 4 个 type-specific 方法
- **`peek()` 直接返回 `&points_[id]`**：真正的零拷贝，前提是调用方在 Seqlock 保护下使用

### 2.2 PointManager::write 实现

```cpp
template<typename T>
bool PointManager::write(PointId id, const T& value) {
    if (!validate_id(id)) return false;

    // Seqlock write begin
    uint64_t seq0 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
    if (seq0 & 1ULL) return false;  // 写冲突
    __atomic_store_n(&header_->write_seq, seq0 + 1, __ATOMIC_RELEASE);

    // 更新数据
    PointData* p = &points_[id];
    if constexpr (std::is_same_v<T, bool>) {
        p->value.b = value; p->type = PointType::BOOL;
    } else if constexpr (std::is_same_v<T, int32_t>) {
        p->value.i = value; p->type = PointType::INT32;
    } else if constexpr (std::is_same_v<T, double>) {
        p->value.d = value; p->type = PointType::DOUBLE;
    } else if constexpr (std::is_same_v<T, const char*>) {
        std::strncpy(p->value.str, value, 31);
        p->value.str[31] = '\0';
        p->type = PointType::STRING;
    }
    p->timestamp_ns = time_->now_ns();
    p->quality = Quality::GOOD;

    // Seqlock write end
    __atomic_store_n(&header_->write_seq, seq0 + 2, __ATOMIC_RELEASE);
    __atomic_fetch_add(&header_->stats.writes, 1, __ATOMIC_RELAXED);
    return true;
}
```

### 2.3 PointManager::peek 实现

```cpp
const PointData* PointManager::peek(PointId id) const {
    if (!validate_id(id)) return nullptr;

    uint64_t s0, s1;
    do {
        s0 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;
        // 可以安全读取 points_[id]——数据在共享内存中
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);

    return &points_[id];  // 真正的零拷贝
}
```

---

### 2.4 SubscriptionManager（定长数组替代 STL）

```cpp
// 订阅回调——进程本地存储，不放入共享内存
using SubscriptionCallback = void (*)(PointId id, const PointData& data, void* user_data);

// 定长订阅条目
struct SubscriberSlot {
    PointId             point_id;
    SubscriptionCallback callback;
    void*               user_data;
    uint64_t            last_heartbeat_ns;
    bool                active;
};

class SubscriptionManager {
public:
    static constexpr size_t MAX_CALLBACKS = 256;

    SubscriptionManager(osal::ITime* time,
                        SubscriberEntry* shm_table,
                        uint32_t max_subscribers);

    bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
    bool unsubscribe(PointId id);

    void notify(PointId id, const PointData& data);
    void update_heartbeat(pid_t pid);
    void cleanup_zombies();

    size_t subscription_count() const;

private:
    SubscriberSlot  slots_[MAX_CALLBACKS];  // 定长数组，零堆分配
    size_t          slot_count_;
    osal::ITime*    time_;

    SubscriberEntry* shm_table_;   // → 共享内存中的心跳表
    uint32_t        max_subscribers_;
};
```

**关键设计决策**：
- **定长 `SubscriberSlot slots_[256]`**：替代 `std::unordered_map<PointId, std::vector<SubscriptionInfo>>`
- **C 风格函数指针**：替代 `std::function`，避免堆分配
- **`shm_table_` 指向共享内存**：心跳信息多进程可见，回调函数本地私有
- **无 `std::mutex`**：使用原子操作保护 slot 分配

### 2.5 SubscriptionManager::notify 实现要点

```cpp
void SubscriptionManager::notify(PointId id, const PointData& data) {
    for (size_t i = 0; i < slot_count_; ++i) {
        if (slots_[i].active && slots_[i].point_id == id) {
            if (slots_[i].callback) {
                slots_[i].callback(id, data, slots_[i].user_data);
            }
        }
    }
}
```

---

### 2.6 SharedMemorySegment（完整实现设计）

```cpp
class SharedMemorySegment {
public:
    SharedMemorySegment(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers);

    bool initialize();       // shm_open + mmap + 初始化 header
    void shutdown();         // munmap + shm_unlink (if owner)
    bool is_owner() const;

    void* base() const;
    InduRTDBHeader* header() const;
    PointData* points() const;
    SubscriberEntry* subscribers() const;

    uint32_t max_points() const;
    uint32_t max_subscribers() const;
    size_t   total_size() const;

private:
    char        name_[64];   // "/indurtdb_<id>"
    void*       base_;       // mmap 基址
    size_t      total_size_;
    bool        is_owner_;

    InduRTDBHeader*  header_;
    PointData*       points_;
    SubscriberEntry* subscribers_;

    uint32_t max_points_;
    uint32_t max_subscribers_;

    std::unique_ptr<osal::ISharedMemory> shm_;  // 保持 OSAL 对象生命周期
};
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

## 4. 文件组织

```
indurtdb/
├── include/indurtdb/
│   ├── types/basic_types.hpp
│   ├── types/memory_layout.hpp
│   ├── core/point_manager_interface.hpp        # PointManager 类（非虚）
│   ├── core/subscription_manager_interface.hpp # SubscriptionManager 类
│   ├── core/seqlock.hpp                        # Seqlock 自由函数
│   ├── core/shared_memory_segment.hpp          # SharedMemorySegment
│   ├── core/config_loader.hpp                  # 配置加载器
│   ├── osal/interface.hpp                      # OSAL 接口
│   ├── osal/factory.hpp                        # OSAL 工厂
│   └── api/indurtdb.hpp                        # InduRTDB 主 API
├── src/
│   ├── api/cpp/indurtdb_impl.cpp
│   ├── api/c/indurtdb_c_impl.cpp
│   ├── core/point_manager.cpp
│   ├── core/shared_memory_segment.cpp
│   ├── core/subscription_manager.cpp
│   ├── core/config_loader.cpp
│   ├── core/seqlock.cpp
│   └── osal/
│       ├── posix/
│       └── sylixos/
├── tests/
│   ├── unit/
│   │   ├── test_seqlock.cpp
│   │   ├── test_subscription_manager.cpp
│   │   ├── test_memory_layout.cpp
│   │   ├── test_basic_types.cpp
│   │   ├── test_alignment.cpp
│   │   ├── test_error.cpp
│   │   └── test_logging.cpp
│   └── integration/
│       └── test_multi_process.cpp
└── CMakeLists.txt
```

---

## 5. 与原设计的关键变更

| 项目 | 原设计 (v1.0) | 新设计 (v2.0) |
|------|-------------|-------------|
| Seqlock | 独立类层次（ISeqlock + Seqlock + Factory） | 3 个自由函数，操作 header_->write_seq |
| PointManager | 虚接口 + `vector<unique_ptr<ISeqlock>>` | 非虚类 + 直接操作共享内存数组 |
| peek() | 复制到 static 临时变量 | 直接返回 `&points_[id]`（零拷贝） |
| SubscriptionManager | `std::unordered_map` + `std::vector` + `std::function` | 定长 `SubscriberSlot[256]` + C 函数指针 |
| 编码规范 | 全面违反（STL/异常/虚函数） | 严格遵守 |

---

**文档变更记录**
- v1.0.0 (2026-03-26)：初始版本
- v2.0.0 (2026-05-11)：移除 STL 依赖，Seqlock 回归全局机制，PointManager 非虚化
