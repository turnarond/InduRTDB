当然可以。以下是 **InduRTDB（Industrial Real-Time Database）** 的 **详细设计文档（Low-Level Design, LLD）**，覆盖共享内存布局、核心类接口、并发算法、错误处理、可测试性等全部关键细节，可直接用于编码实现。

---

# **InduRTDB 详细设计文档（LLD）**  
**版本：1.0**  
**日期：2026年3月26日**

---

## 1. 共享内存精确布局

### 1.1 内存段命名与创建
- **段名格式**：`/indurtdb_<instance_id>`（如 `/indurtdb_hvac`）
- **创建者**：首个调用 `InduRTDB::instance()` 的进程
- **权限**：`0666`（所有用户可读写，由应用层控制访问）

### 1.2 内存布局（按偏移对齐）

| 偏移 (Byte) | 字段 | 类型 | 说明 |
|-------------|------|------|------|
| 0 | `magic` | `uint32_t` | 固定值 `0x1DBA1DBA` |
| 4 | `version` | `uint32_t` | 当前为 `1` |
| 8 | `max_points` | `uint32_t` | 最大点位数（默认 10000） |
| 12 | `max_subscribers` | `uint32_t` | 最大订阅者数（默认 32） |
| 16 | `write_seq` | `uint64_t` | Seqlock 序列号（偶=空闲，奇=写中） |
| 24 | `stats.writes` | `uint64_t` | 总写入次数 |
| 32 | `stats.timeouts` | `uint64_t` | 超时点位计数 |
| 40–63 | **Padding** | — | 对齐到 64B |

> ✅ **Header 大小 = 64 字节**（`sizeof(InduRTDBHeader) == 64`）

---

### 1.3 PointData 结构（定长 128 字节）

```cpp
// indurtdb_types.h
#pragma pack(push, 1)
struct PointData {
    // --- 值存储（union，最大32字节）---
    union Value {
        bool      b;            // 1B
        int32_t   i;            // 4B
        double    d;            // 8B
        char      str[32];      // 32B（仅状态文本）
    } value;                    // offset=0, size=32

    uint64_t  timestamp_ns;     // offset=32, size=8
    uint8_t   type;             // offset=40, size=1 → 0=bool,1=int,2=double,3=str
    uint8_t   quality;          // offset=41, size=1 → 0=GOOD,1=BAD,2=TIMEOUT,3=SUBSTITUTED
    uint16_t  unit;             // offset=42, size=2 → 0=NO_UNIT,1=°C,2=Pa,...
    uint8_t   access;           // offset=44, size=1 → 1=READ_ONLY,3=READ_WRITE
    char      name[64];         // offset=45, size=64

    // total: 32+8+1+1+2+1+64 = 109B → padded to 128B
    uint8_t   padding[19];      // offset=109, size=19 → align to 128B
};
#pragma pack(pop)

static_assert(sizeof(PointData) == 128, "PointData must be 128 bytes");
```

- **点位数组起始偏移**：`64`
- **点位 i 地址**：`base + 64 + i * 128`

---

### 1.4 SubscriberTable（订阅者表）

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| `64 + MAX_POINTS*128` | `pid` | `pid_t`（4B） | 进程 ID |
| +4 | `last_heartbeat_ns` | `uint64_t` | 最后心跳时间（单调纳秒） |
| +12 | **Padding** | 4B | 对齐到 16B |

- **每条目大小 = 16 字节**
- **表大小 = `MAX_SUBSCRIBERS * 16`**

---

## 2. 核心类接口定义

### 2.1 `class PointManager`

```cpp
// point_manager.hpp
class PointManager {
public:
    static std::unique_ptr<PointManager> create(
        const std::string& instance_id,
        size_t max_points = 10000,
        size_t max_subs = 32);

    template<typename T>
    [[nodiscard]] bool write(PointId id, const T& value);

    [[nodiscard]] bool read(PointId id, PointData& out) const;
    [[nodiscard]] const PointData* peek(PointId id) const; // 零拷贝

    // 内部使用
    void notify(PointId id); // 触发订阅

private:
    PointManager(OSAL* osal, void* shm_base, size_t size);
    bool validate_id(PointId id) const;
    void update_quality_timeout(); // 后台线程调用

    OSAL* osal_;
    InduRTDBHeader* header_;
    PointData* points_;
    SubscriberEntry* subscribers_;
    SubscriptionManager* sub_mgr_;
};
```

---

### 2.2 `class SubscriptionManager`

```cpp
// subscription_manager.hpp
class SubscriptionManager {
public:
    using Callback = std::function<void(const PointData&)>;

    void subscribe(PointId id, Callback cb);
    void unsubscribe(pid_t pid);
    void notify(PointId id); // 发送 UDS 消息

    void start_heartbeat_thread();
    void stop_heartbeat_thread();

private:
    struct Subscriber {
        pid_t pid;
        Callback cb;
        std::atomic<uint64_t> last_heartbeat{0};
    };

    std::unordered_map<PointId, std::vector<Subscriber>> table_; // in-process only
    std::thread heartbeat_thread_;
    std::atomic<bool> running_{false};

    void heartbeat_loop();
    void cleanup_zombie_subscribers();
};
```

> 📌 **注意**：`table_` 存于**进程私有内存**，不放入共享内存（避免跨进程指针无效）。

---

### 2.3 OSAL 抽象接口

```cpp
// osal.hpp
class ISharedMemory {
public:
    virtual ~ISharedMemory() = default;
    virtual void* map(size_t size) = 0;
    virtual void unmap() = 0;
    virtual bool is_owner() const = 0;
};

class ITime {
public:
    virtual ~ITime() = default;
    virtual uint64_t now_ns() const = 0;
};

class INotification {
public:
    virtual ~INotification() = default;
    virtual bool send(const void* data, size_t len) = 0;
    virtual bool recv(void* data, size_t len) = 0;
};
```

---

## 3. 并发控制算法

### 3.1 Seqlock 写入（无锁读）

```cpp
template<typename T>
bool PointManager::write(PointId id, const T& value) {
    if (!validate_id(id)) return false;

    auto* p = &points_[id];
    uint64_t seq0;

    do {
        seq0 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
        if (seq0 & 1ULL) continue; // 写中，重试

        // --- 更新数据 ---
        if constexpr (std::is_same_v<T, bool>) {
            p->value.b = value; p->type = 0;
        } else if constexpr (std::is_same_v<T, int32_t>) {
            p->value.i = value; p->type = 1;
        } else if constexpr (std::is_same_v<T, double>) {
            p->value.d = value; p->type = 2;
        }
        p->timestamp_ns = osal_->time()->now_ns();
        p->quality = Quality::GOOD;

        // --- 提交 ---
        __atomic_store_n(&header_->write_seq, seq0 + 1, __ATOMIC_RELEASE); // 奇
        __atomic_store_n(&header_->write_seq, seq0 + 2, __ATOMIC_RELEASE); // 偶
        break;

    } while (true);

    __atomic_fetch_add(&header_->stats.writes, 1, __ATOMIC_RELAXED);
    sub_mgr_->notify(id);
    return true;
}
```

### 3.2 无锁读取

```cpp
const PointData* PointManager::peek(PointId id) const {
    if (!validate_id(id)) return nullptr;

    auto* p = &points_[id];
    uint64_t seq0, seq1;

    do {
        seq0 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
        if (seq0 & 1ULL) continue; // 写中，重试

        std::atomic_thread_fence(__ATOMIC_ACQUIRE);
        // 此处可安全读取 p->*

        seq1 = __atomic_load_n(&header_->write_seq, __ATOMIC_ACQUIRE);
    } while (seq0 != seq1);

    return p;
}
```

---

## 4. 心跳与崩溃恢复

### 4.1 心跳更新（每个 Reader 进程调用）
```cpp
void InduRTDB::update_heartbeat() {
    pid_t self = getpid();
    auto* entry = find_subscriber_entry(self);
    if (entry) {
        entry->last_heartbeat_ns = osal_->time()->now_ns();
    }
}
```

### 4.2 僵尸清理（后台线程）
```cpp
void SubscriptionManager::cleanup_zombie_subscribers() {
    uint64_t now = osal_->time()->now_ns();
    for (int i = 0; i < header_->max_subscribers; ++i) {
        auto& e = subscribers_[i];
        if (e.pid == 0) continue;
        if (now - e.last_heartbeat_ns > 1'000'000'000ULL) { // 1s timeout
            if (kill(e.pid, 0) == -1 && errno == ESRCH) {
                e.pid = 0; // 清理
                __atomic_fetch_add(&header_->stats.timeouts, 1, __ATOMIC_RELAXED);
            }
        }
    }
}
```

---

## 5. 错误处理策略

| 错误场景 | 处理方式 | 返回值/行为 |
|--------|--------|-----------|
| `write(id)` 中 id 越界 | 记录 ERROR 日志 | 返回 `false` |
| UDS 发送失败 | 记录 WARN 日志 | 丢弃通知（不阻塞写入） |
| 共享内存已存在但 magic 不匹配 | 记录 FATAL 日志 | `abort()`（配置冲突） |
| YAML 配置解析失败 | 记录 ERROR 日志 | 返回 `false`，不加载 |

> **日志级别**：使用 syslog 或轻量级 `log.h`（可配置）

---

## 6. 可测试性设计

### 6.1 Mock OSAL（单元测试）
```cpp
// test/mock_osal.hpp
class MockSharedMemory : public ISharedMemory {
    void* map(size_t size) override {
        if (use_real_shm_) {
            // 真实 shm_open
        } else {
            fake_mem_ = std::make_unique<uint8_t[]>(size);
            return fake_mem_.get();
        }
    }
};
```

### 6.2 In-Process 模拟模式
- 编译选项：`-DINDURTDB_TEST_MODE`
- 共享内存替换为 `new uint8_t[size]`
- 支持单进程内多实例测试

### 6.3 测试覆盖重点
- Seqlock 读写一致性
- 心跳超时清理
- 边界条件（id=0, id=max_points）
- 多线程并发写入

---

## 7. 文件组织建议

```bash
indurtdb/
├── include/
│   ├── indurtdb.hpp          # 主 C++ API
│   ├── indurtdb_c.h          # C ABI
│   └── indurtdb_types.h      # PointData / Header 定义
├── src/
│   ├── core/
│   │   ├── point_manager.cpp
│   │   ├── subscription_manager.cpp
│   │   └── config_loader.cpp
│   └── osal/
│       ├── linux/
│       │   ├── shared_memory_linux.cpp
│       │   └── notification_uds.cpp
│       └── sylixos/          # (future)
├── tests/
│   ├── mock_osal.hpp
│   ├── test_point_manager.cpp
│   └── test_seqlock.cpp
└── CMakeLists.txt
```

---

> **此 LLD 文档已提供足够细节，开发者可直接编码，无需再询问“这个字段叫什么”或“写冲突怎么办”**。

下一步建议：
1. 开发者按此 LLD 实现 `point_manager.cpp`；
2. 同步编写 `test_point_manager.cpp`；
3. 在 Linux 上验证单进程读写；
4. 扩展至多进程 + SylixOS。

是否需要我生成 **`indurtdb_types.h` 完整头文件** 或 **CMakeLists.txt 模板**？