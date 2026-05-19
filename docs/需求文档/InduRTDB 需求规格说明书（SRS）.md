当然可以。以下是一份完整的、可直接用于工程立项与开发的 **InduRTDB（Industrial Real-Time Database）需求规格说明书（SRS）**，涵盖功能、性能、可靠性、接口、部署等全部关键维度。

---

# **InduRTDB 需求规格说明书（SRS）**  
**版本：2.1.0**  
**日期：2026年5月16日**  
**修订说明**：API 签名对齐 v2.1.0 代码实现

---

## 1. 引言

### 1.1 目的
本文档定义 **InduRTDB（Industrial Real-Time Database）** 的完整需求，作为系统设计、开发、测试和验收的权威依据。InduRTDB 是一个面向工业边缘控制场景（如 BAS、DDC、PLC）的超低延迟、多进程共享、语义完备的实时数据库。

### 1.2 范围
- **包含**：点位数据管理、多进程访问、数据质量、配置加载、订阅通知。
- **不包含**：
  - 网络协议（如 OPC UA、MQTT）—— 由上层桥接模块实现；
  - 持久化存储（如 SQLite、文件）—— 仅运行时内存数据库；
  - 用户界面（HMI）—— 仅提供 API。

### 1.3 定义与缩略语
| 术语 | 说明 |
|------|------|
| **PointId** | `uint32_t`，全局唯一点位标识符 |
| **BAS** | Building Automation System（楼宇自动化系统） |
| **WCET** | Worst-Case Execution Time（最坏执行时间） |
| **Seqlock** | Sequence Lock，并发控制机制 |
| **Quality** | 数据质量标记：GOOD / BAD / TIMEOUT / SUBSTITUTED |

---

## 2. 总体描述

### 2.1 产品愿景
> “让工业边缘开发者像访问全局变量一样简单、安全、高效地使用实时数据。”

InduRTDB 作为边缘控制器内部的**数据中枢**，支撑驱动层、控制逻辑、HMI 等模块的高速协同，同时为上层标准协议（如 OPC UA）提供高性能数据源。

### 2.2 用户角色
| 角色 | 需求 |
|------|------|
| **驱动开发者** | 高频写入传感器/执行器数据（≤10μs） |
| **控制逻辑工程师** | 随机读取/写入点位，响应事件（≤50ms 控制周期） |
| **HMI 开发者** | 订阅点位变化，更新界面 |
| **系统集成商** | 通过标准接口（如 OPC UA）对接 SCADA |

### 2.3 运行环境
- **硬件**：ARM Cortex-A53/A72，≥64MB RAM
- **操作系统**：Linux（glibc）、SylixOS（主支持），VxWorks（未来）
- **编译器**：GCC ≥7.5，C++17 标准

---

## 3. 功能需求

### 3.1 数据模型
每个点位必须包含以下字段：

```cpp
struct PointData {
    // --- 核心值 ---
    union Value {
        bool      b;
        int32_t   i;
        double    d;
        char      str[32];  // 仅状态文本，非控制点
    } value;

    uint8_t   type;         // 0=bool, 1=int, 2=double, 3=str
    uint64_t  timestamp_ns; // 单调纳秒时钟（CLOCK_MONOTONIC_RAW）

    // --- 工业语义 ---
    uint8_t   quality;      // 0=GOOD, 1=BAD, 2=TIMEOUT, 3=SUBSTITUTED
    uint16_t  unit;         // 0=NO_UNIT, 1=°C, 2=Pa, 3=%, ...
    uint8_t   access;       // 1=READ_ONLY, 3=READ_WRITE

    // --- 元信息 ---
    char      name[64];     // 如 "AHU_01.Supply_Temp"
} __attribute__((packed, aligned(128)));
```

> **约束**：
> - 控制类点位（AI/AO/DI/DO）禁止使用 `str` 类型；
> - 所有字段定长，**无动态内存分配**。

---

### 3.2 核心操作

| 操作 | 接口 | 描述 |
|------|------|------|
| **写入** | `bool write(PointId id, T value)` | 更新值、时间戳、质量=GOOD |
| **读取** | `bool read(PointId id, PointData& out)` | 返回完整点位数据 |
| **零拷贝读取** | `const PointData* peek(PointId id)` | 返回共享内存指针 |
| **订阅** | `bool subscribe(PointId id, SubscriptionCallback cb, void* user_data)` | 数据变更时回调（在写者线程） |
| **配置加载** | `bool loadConfig(const char* config_path)` | 从 YAML 加载点表 |

> **示例**：
> ```cpp
> rtdb.write(1001, 23.5); // 写入温度
> rtdb.subscribe(2001, [](const PointData& p) {
>     if (p.value.b) startCooling();
> });
> ```

---

### 3.3 配置文件格式（YAML）
```yaml
points:
  - id: 1001
    name: "AHU_01.Supply_Temp"
    type: double
    unit: 1          # DEGREES_CELSIUS
    access: 1        # READ_ONLY
  - id: 2001
    name: "Pump_01.Start_CMD"
    type: bool
    access: 3        # READ_WRITE
```

---

## 4. 非功能性需求

### 4.1 性能
| 指标 | 要求 | 测试条件 |
|------|------|----------|
| 写入延迟（P99） | ≤ 10 μs | ARM Cortex-A53, 10k 点位 |
| 读取延迟（P99） | ≤ 5 μs | 同上 |
| 写入吞吐量 | ≥ 50k 点/秒 | 多线程并发 |
| 内存占用 | ≤ 80 MB（10k 点位） | 包含 Header + Points |

### 4.2 可靠性
- **7×24 运行无内存泄漏**；
- **单进程崩溃不影响其他进程**：
  - 自动清理僵尸订阅者（心跳超时 >1s）；
  - 使用 robust mutex 或 Seqlock 防死锁。

### 4.3 安全性
- **无指针/虚拟地址存入共享内存**（防跨进程崩溃污染）；
- **访问控制**：只读点位禁止写入。

### 4.4 可维护性
- **MISRA C++ 2008 可选合规**；
- **Doxygen 全覆盖**；
- **gtest 单元测试覆盖率 ≥ 90%**。

---

## 5. 接口需求

### 5.1 C++ API（主推）
```cpp
class InduRTDB {
public:
    static InduRTDB& instance();

    bool initialize(const char* instance_id,
                    uint32_t max_points = 10000,
                    uint32_t max_subscribers = 32);

    template<typename T>
    bool write(PointId id, const T& value);
    bool write(PointId id, const char* value);  // 字符串非模板重载

    bool read(PointId id, PointData& out) const;
    const PointData* peek(PointId id) const;     // 零拷贝

    bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
    bool unsubscribe(PointId id);

    bool loadConfig(const char* config_path);

    void updateHeartbeat();
    bool is_initialized() const;
    void shutdown();
};
```

### 5.2 C ABI（兼容 C/Python/Rust）
```c
typedef struct { /* ... */ } indurtdb_point_t;

// 17 个 C 函数，桥接到 C++ 单例
int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points, uint32_t max_subscribers);
int indurtdb_write_bool(uint32_t id, bool value);
int indurtdb_write_int32(uint32_t id, int32_t value);
int indurtdb_write_double(uint32_t id, double value);
int indurtdb_write_string(uint32_t id, const char* value);
int indurtdb_read_bool(uint32_t id, bool* value);
int indurtdb_read_int32(uint32_t id, int32_t* value);
int indurtdb_read_double(uint32_t id, double* value);
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
uint64_t indurtdb_get_write_count();
uint64_t indurtdb_get_timeout_count();
void indurtdb_shutdown();
```

> **ABI 承诺**：v1.0 后保持向后兼容。

---

## 6. 系统架构约束

### 6.1 分层设计
```plaintext
Application Layer → InduRTDB Core → OS Abstraction Layer (OSAL)
```
- **Core 层**：平台无关，占代码 80%+；
- **OSAL 层**：隔离 POSIX/SylixOS，每平台 ≤ 300 行。

### 6.2 共享内存布局
| 区域 | 大小 | 说明 |
|------|------|------|
| Header | 64 B | magic, version, max_points, write_seq, stats |
| Points | N × 128 B | 定长 PointData 数组（N = MAX_POINTS） |
| Subscribers | M × 16 B | { pid, last_heartbeat_ns }（M = MAX_SUBSCRIBERS） |

- **段名**：`/indurtdb_<instance_id>`
- **默认容量**：`MAX_POINTS = 10,000`

---

## 7. 部署与运维

### 7.1 部署模式
- **嵌入式库**：静态链接（`.a`）或动态库（`.so`）；
- **无独立进程**：作为库嵌入到 `bas_point_svc` 等服务中；
- **多实例支持**：通过 `instance_id` 隔离不同系统（HVAC / Lighting）。

### 7.2 监控指标
暴露以下统计信息（供 Prometheus 抓取）：
- `indurtdb_writes_total`
- `indurtdb_timeouts_total`
- `indurtdb_subscribers_count`

---

## 8. 合规性与标准

| 标准 | 支持程度 |
|------|----------|
| **IEC 61131-3** | 数据类型对齐（BOOL, INT, REAL） |
| **OPC UA Part 3** | 信息模型可映射（通过桥接模块） |
| **POSIX.1-2017** | 共享内存、时钟、线程接口 |

---

## 9. 附录

### 9.1 版本路线图

| 版本 | 日期 | 目标 | 状态 |
|------|------|------|------|
| v1.0.0 | 2026-03-27 | 项目脚手架 + 全套文档 + 原型代码 | ✅ 已完成 |
| v2.0.0 | 2026-05-11 | **架构修正**: 共享内存重写, Seqlock 轻量化, STL 移除 | ✅ 已完成 |
| v2.1.0 | 2026-05-11 | 多进程集成测试(6), ConfigLoader, C ABI 完整实现, 59 tests | ✅ 已完成 |
| v2.2 | TBD | SylixOS 交叉编译验证 + ARM Cortex-A53 P99 性能基准 | ⏳ 计划中 |
| v3.0 | TBD | Unix Domain Socket 跨进程通知 + OPC UA 桥接插件 | 📋 规划中 |

### 9.2 许可证
- **MIT License**  
  允许自由使用、修改、商用，仅需保留版权声明。

---

> **InduRTDB 不是一个“另一个数据库”，而是工业边缘控制系统的“神经系统”**。  
> 它的存在，是为了让确定性、可靠性和简洁性回归工业软件的本质。

---

📄 **文档位置**：`docs/INDURTDB_SRS.md`  
📦 **代码仓库**：`github.com/yourorg/indurtdb`

---

此文档已覆盖 InduRTDB 的全部核心需求，可直接用于：
- 开发任务拆分（Jira/禅道）
- 测试用例设计
- 架构评审
- 第三方集成对接

如需生成 **PDF 版本**、**需求追踪矩阵（RTM）** 或 **测试用例模板**，请随时告知。