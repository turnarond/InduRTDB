# InduRTDB 工程框架架构设计文档

**版本：1.1.0**
**日期：2026年5月11日**
**修订说明**：Core 层去虚函数、去 STL；PointManager 改为模板接口；Seqlock 回归轻量自由函数

## 1. 文档概述

### 1.1 文档目的
本文档详细描述了InduRTDB（Industrial Real-Time Database）工程框架的整体架构设计，包括分层结构、模块划分、类关系、数据流和关键技术选型。本设计严格遵循项目编码规范，确保框架具备良好的可扩展性、可维护性和可测试性。

### 1.2 设计原则
- **确定性优先**：性能可预测，执行时间确定
- **可靠性至上**：7×24小时稳定运行，无内存泄漏
- **简洁性核心**：代码简洁明了，避免过度设计
- **平台无关性**：核心层100%平台无关，OSAL层隔离平台差异

### 1.3 设计约束
- **零动态内存分配**：禁止`new`/`delete`、`malloc`/`free`、STL容器
- **零异常**：编译选项`-fno-exceptions -fno-rtti`
- **零阻塞系统调用**：所有IPC必须非阻塞或带超时
- **确定性执行时间**：避免虚函数、递归、复杂模板展开

## 2. 整体架构

### 2.1 四层架构设计

InduRTDB采用**四层分层架构**，严格隔离关注点：

```
┌─────────────────────────────────────────────────────────────┐
│                   应用层 (Application Layer)                  │
│  • BAS驱动 (Modbus, BACnet)                                │
│  • 控制逻辑引擎 (PID, Sequencer)                           │
│  • HMI / Web UI                                           │
│  • OPC UA Server桥接                                      │
│                                                           │
│  ← 使用InduRTDB C++/C API进行读写/订阅操作                  │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│                   API层 (API Layer)                         │
│  ┌─────────────────┐  ┌─────────────────┐                  │
│  │   C++ API      │  │    C ABI        │                  │
│  │  (主推)        │  │  (兼容C/Python/Rust)              │
│  └─────────┬───────┘  └─────────────────┘                  │
└────────────┼───────────────────────────────────────────────┘
             │
┌────────────▼───────────────────────────────────────────────┐
│                   核心层 (Core Layer)                       │
│  ┌─────────────────┐    ┌─────────────────┐               │
│  │ PointManager    │    │ SubscriptionMgr │               │
│  │ • write()       │    │ • subscribe()   │               │
│  │ • read()        │    │ • notify()      │               │
│  └─────────┬───────┘    └─────────┬───────┘               │
│            │                      │                       │
│  ┌─────────▼──────────────────────▼─────────┐             │
│  │       SharedMemorySegment                │             │
│  │ • map()                                 │             │
│  │ • unmap()                               │             │
│  └─────────────────────────────────────────┘             │
└────────────┬───────────────────────────────────────────────┘
             │
┌────────────▼───────────────────────────────────────────────┐
│                 OS抽象层 (OSAL Layer)                       │
│  ┌─────────────────┐  ┌─────────────────┐                 │
│  │ ISharedMemory   │  │   IThreading    │                 │
│  │ (接口)          │  │  (接口)         │                 │
│  └─────────┬───────┘  └─────────┬───────┘                 │
│            │                    │                         │
│  ┌─────────▼───────┐  ┌─────────▼───────┐                 │
│  │ Linux实现       │  │ SylixOS实现     │                 │
│  │ (具体实现)      │  │ (具体实现)      │                 │
│  └─────────────────┘  └─────────────────┘                 │
└───────────────────────────────────────────────────────────┘
```

### 2.2 各层职责

| 层级 | 职责 | 关键技术 |
|------|------|----------|
| **应用层** | 使用InduRTDB API的业务应用 | C++/C API调用 |
| **API层** | 提供统一的编程接口 | 单例模式、PIMPL模式 |
| **核心层** | 平台无关的核心逻辑 | Seqlock、共享内存、回调机制 |
| **OSAL层** | 隔离操作系统差异 | 工厂模式、接口抽象 |

## 3. 模块划分

### 3.1 目录结构

```
indurtdb/
├── include/                    # 公共头文件
│   ├── indurtdb/              # 主命名空间
│   │   ├── api/               # API接口
│   │   ├── types/             # 类型定义
│   │   ├── core/              # 核心接口
│   │   ├── osal/              # OS抽象接口
│   │   └── utils/             # 工具接口
│   └── indurtdb.hpp           # 主包含文件
├── src/                       # 源代码
│   ├── api/                   # API实现层
│   │   ├── cpp/               # C++ API实现
│   │   └── c/                 # C ABI实现
│   ├── core/                  # 核心层
│   │   ├── point_manager.cpp  # 点位管理
│   │   ├── shared_memory_segment.cpp # 共享内存段
│   │   ├── subscription_manager.cpp  # 订阅管理
│   │   └── config_loader.cpp  # 配置加载
│   ├── osal/                  # OS抽象层
│   │   ├── interface/         # 抽象接口实现
│   │   ├── linux/             # Linux实现
│   │   └── sylixos/           # SylixOS实现
│   └── utils/                 # 工具层
│       ├── logging.cpp        # 日志系统
│       ├── error.cpp          # 错误处理
│       └── alignment.cpp      # 内存对齐
├── tests/                     # 测试代码
│   ├── unit/                  # 单元测试
│   ├── integration/           # 集成测试
│   └── performance/           # 性能测试
├── examples/                  # 示例代码
│   ├── basic/                 # 基本使用示例
│   └── advanced/              # 高级用法示例
└── docs/                      # 文档
    ├── 设计文档/              # 设计文档
    └── 需求文档/              # 需求文档
```

### 3.2 核心模块

#### 3.2.1 类型系统模块
- **文件**：`include/indurtdb/types/`
- **职责**：定义所有基础类型和数据结构
- **关键类**：
  - `PointId`：点位标识符（uint32_t）
  - `PointData`：点位数据结构（128字节对齐）
  - `InduRTDBHeader`：共享内存头部（64字节对齐）

#### 3.2.2 共享内存管理模块
- **文件**：`src/core/shared_memory_segment.cpp`
- **职责**：管理共享内存的创建、映射和释放
- **关键技术**：
  - POSIX共享内存（Linux）
  - 内存对齐（64字节/128字节）
  - 魔术字验证

#### 3.2.3 点位管理模块
- **文件**：`src/core/point_manager.cpp`
- **职责**：提供点位的读写操作（非虚类，编译期绑定）
- **关键技术**：
  - 全局 Seqlock（Header.write_seq）并发控制
  - 无锁读取（peek 零拷贝返回共享内存指针）
  - 模板 write<T> 编译期类型分发
  - GCC `__atomic_*` 内建函数

#### 3.2.4 订阅管理模块
- **文件**：`src/core/subscription_manager.cpp`
- **职责**：管理点位变更订阅
- **关键技术**：
  - 定长 SubscriberSlot 数组（零堆分配）
  - C 风格函数指针回调
  - 共享内存心跳表 + 僵尸进程清理
  - Unix Domain Socket 通知（跨进程）

#### 3.2.5 OS抽象层模块
- **文件**：`src/osal/`
- **职责**：隔离操作系统差异
- **关键技术**：
  - 工厂模式创建平台实例
  - 接口抽象（ISharedMemory、ITime等）
  - 平台特定实现（Linux、SylixOS）

## 4. 类关系设计

### 4.1 核心类图

```
┌─────────────────────────────────────────────────────────────┐
│                      InduRTDB (Singleton)                    │
│  - instance(): InduRTDB&                                    │
│  - initialize(): bool                                       │
│  - write<T>(): bool                                         │
│  - read(): bool                                             │
│  - subscribe(): bool                                        │
│  - shutdown(): void                                         │
│                                                             │
│  ┌─────────────────────────────────────────────────────┐   │
│  │                    Impl (PIMPL)                     │   │
│  │  - segment_: SharedMemorySegment                    │   │
│  │  - point_mgr_: PointManager                         │   │
│  │  - sub_mgr_: SubscriptionManager                    │   │
│  └─────────────────────────────────────────────────────┘   │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│                 PointManager (Concrete, 非虚)               │
│  - write<T>(id, value): bool                                │
│  - read(id, out): bool                                      │
│  - peek(id): const PointData*    ← 零拷贝，返回 &points_[id]│
│  - validate_id(id): bool                                    │
│                                                             │
│  - header_: InduRTDBHeader*    ← 共享内存头部                │
│  - points_: PointData*         ← 共享内存点位数组            │
│  - time_: ITime*               ← OSAL 时间接口               │
└──────────────────────────────┬──────────────────────────────┘
                               │
┌──────────────────────────────▼──────────────────────────────┐
│              SharedMemorySegment (Concrete)                  │
│  - initialize(): bool    ← shm_open + mmap + init header    │
│  - shutdown(): void      ← munmap + shm_unlink              │
│  - base(): void*                                            │
│  - header(): InduRTDBHeader*                                │
│  - points(): PointData*                                     │
│  - subscribers(): SubscriberEntry*                          │
└─────────────────────────────────────────────────────────────┘
```

> **Core 层设计原则**：
> - **非虚类**：PointManager、SharedMemorySegment 不需要多态，编译期绑定
> - **零 STL**：定长数组替代 vector/map/unordered_map
> - **零异常**：全部 bool 返回值
> - **Seqlock 为轻量自由函数**：操作 header_->write_seq，不封装为类层次

### 4.2 接口设计

#### 4.2.1 OSAL接口
```cpp
// 共享内存接口
class ISharedMemory {
    virtual void* map(size_t size) = 0;
    virtual void unmap() = 0;
    virtual bool is_owner() const = 0;
};

// 时间接口
class ITime {
    virtual TimestampNs now_ns() const = 0;
    virtual void sleep_ns(TimestampNs duration) const = 0;
};

// 线程接口
class IThreading {
    virtual void set_affinity(int cpu_core) = 0;
    virtual void yield() = 0;
};

// 通知接口
class INotification {
    virtual bool send(const void* data, size_t size) = 0;
    virtual bool receive(void* data, size_t size, TimestampNs timeout_ns) = 0;
};
```

#### 4.2.2 核心接口（非虚，编译期绑定）

```cpp
// PointManager —— 直接操作共享内存，无虚函数
class PointManager {
public:
    PointManager(void* shm_base, uint32_t max_points, osal::ITime* time);

    // 模板写入（满足 SRS：rtdb.write(id, value)）
    template<typename T>
    bool write(PointId id, const T& value);

    bool read(PointId id, PointData& out) const;
    const PointData* peek(PointId id) const;  // 零拷贝

    bool validate_id(PointId id) const;
    uint64_t get_write_count() const;

private:
    InduRTDBHeader* header_;
    PointData*      points_;
    uint32_t        max_points_;
    osal::ITime*    time_;
};

// Seqlock —— 轻量自由函数（非类）
uint64_t seqlock_write_begin(uint64_t* seq);
void     seqlock_write_end(uint64_t* seq, uint64_t seq0);
const PointData* seqlock_read(const uint64_t* seq,
                               const PointData* points, uint32_t id);
```

## 5. 数据流设计

### 5.1 写入数据流

```
┌─────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ 驱动层   │───▶│ PointManager │───▶│ 共享内存     │───▶│ 订阅通知     │
│ (Writer)│    │              │    │              │    │              │
└─────────┘    └──────────────┘    └──────────────┘    └──────────────┘
     │                │                    │                    │
     ▼                ▼                    ▼                    ▼
┌─────────┐    ┌──────────────┐    ┌──────────────┐    ┌──────────────┐
│ 写入请求 │───▶│ Seqlock开始  │───▶│ 更新数据     │───▶│ UDS发送      │
│         │    │              │    │              │    │              │
└─────────┘    └──────────────┘    └──────────────┘    └──────────────┘
```

### 5.2 读取数据流

```
┌─────────┐    ┌──────────────┐    ┌──────────────┐
│ 应用层   │───▶│ PointManager │───▶│ 共享内存     │
│ (Reader)│    │              │    │              │
└─────────┘    └──────────────┘    └──────────────┘
     │                │                    │
     ▼                ▼                    ▼
┌─────────┐    ┌──────────────┐    ┌──────────────┐
│ 读取请求 │───▶│ Seqlock读取  │───▶│ 零拷贝返回   │
│         │    │              │    │              │
└─────────┘    └──────────────┘    └──────────────┘
```

## 6. 并发控制设计

### 6.1 Seqlock算法

#### 6.1.1 写入流程
```cpp
bool write(PointId id, T value) {
    // 1. 读取序列号
    uint64_t seq0 = atomic_load(&header->write_seq);
    
    // 2. 检查是否正在写入（奇数表示正在写入）
    if (seq0 & 1) return false;
    
    // 3. 增加序列号（标记开始写入）
    atomic_store(&header->write_seq, seq0 + 1);
    
    // 4. 写入数据
    update_point_data(id, value);
    
    // 5. 增加序列号（标记写入完成）
    atomic_store(&header->write_seq, seq0 + 2);
    
    return true;
}
```

#### 6.1.2 读取流程
```cpp
const PointData* read(PointId id) {
    uint64_t seq0, seq1;
    const PointData* data;
    
    do {
        // 1. 读取序列号
        seq0 = atomic_load(&header->write_seq);
        
        // 2. 检查是否正在写入
        if (seq0 & 1) continue;
        
        // 3. 内存屏障
        atomic_thread_fence(acquire);
        
        // 4. 读取数据
        data = &points[id];
        
        // 5. 再次读取序列号
        seq1 = atomic_load(&header->write_seq);
        
    } while (seq0 != seq1); // 6. 如果序列号变化，重试
    
    return data;
}
```

### 6.2 性能特性
- **读操作**：完全无锁，无阻塞
- **写操作**：低开销，仅需两次原子操作
- **冲突处理**：读-写冲突时，读操作重试
- **写-写冲突**：极罕见，返回失败

## 7. 内存布局设计

### 7.1 共享内存结构

```
偏移量     大小        字段                说明
0          4字节       magic             魔术字 0x1DBA1DBA
4          4字节       version           版本号
8          4字节       max_points        最大点位数
12         4字节       max_subscribers   最大订阅者数
16         8字节       write_seq         Seqlock序列号
24         8字节       stats.writes      总写入次数
32         8字节       stats.timeouts    超时点位计数
40         24字节      padding           填充到64字节

64         128×N字节   points[]          点位数据数组
64+128×N   16×M字节    subscribers[]     订阅者表
```

### 7.2 点位数据结构

```
偏移量     大小        字段                说明
0          32字节      value             值联合体
32         8字节       timestamp_ns      时间戳（纳秒）
40         1字节       type              数据类型
41         1字节       quality           数据质量
42         2字节       unit              单位
44         1字节       access            访问权限
45         64字节      name              点位名称
109        19字节      padding           填充到128字节
```

## 8. 错误处理设计

### 8.1 错误码体系

```cpp
enum class ErrorCode {
    SUCCESS = 0,
    INVALID_ARGUMENT,    // 参数无效
    OUT_OF_RANGE,        // 超出范围
    IO_ERROR,            // I/O错误
    MEMORY_ERROR,        // 内存错误
    TIMEOUT,             // 超时
    NOT_INITIALIZED,     // 未初始化
    ALREADY_INITIALIZED, // 已初始化
    NOT_FOUND,           // 未找到
    ALREADY_EXISTS,      // 已存在
    PERMISSION_DENIED,   // 权限拒绝
    NOT_SUPPORTED,       // 不支持
    INTERNAL_ERROR,      // 内部错误
    UNKNOWN_ERROR        // 未知错误
};
```

### 8.2 错误处理策略
- **返回码而非异常**：所有函数返回`bool`或`ErrorCode`
- **线程局部错误信息**：每个线程维护自己的错误状态
- **系统错误码映射**：将系统错误码映射为项目错误码
- **错误信息本地化**：支持多语言错误信息

## 9. 测试策略

### 9.1 测试层次

| 测试类型 | 覆盖率要求 | 测试工具 | 测试重点 |
|----------|------------|----------|----------|
| **单元测试** | ≥90%函数覆盖率 | Google Test | 单个函数/类 |
| **集成测试** | 100%接口覆盖 | Google Test | 模块间交互 |
| **并发测试** | 必须包含 | 自定义测试 | 多线程安全 |
| **性能测试** | P99≤10μs | perf/cyclictest | 延迟/吞吐量 |
| **崩溃恢复测试** | 必须包含 | kill -9模拟 | 进程崩溃恢复 |

### 9.2 测试框架
- **测试框架**：Google Test
- **Mock框架**：Google Mock（用于OSAL接口）
- **覆盖率工具**：gcov/lcov
- **性能分析**：perf, ftrace

## 10. 构建与部署

### 10.1 构建系统
- **构建工具**：CMake ≥ 3.15
- **编译选项**：
  ```bash
  -O2 -g -Wall -Wextra -Werror
  -fno-exceptions -fno-rtti
  -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections
  ```
- **目标平台**：Linux (glibc)、SylixOS
- **目标硬件**：ARM Cortex-A53/A72，≥64MB RAM

### 10.2 部署模式
- **嵌入式库模式**：静态链接（`.a`）到应用
- **无独立进程**：作为库嵌入到服务中
- **多实例支持**：通过`instance_id`隔离不同系统
- **监控指标**：通过共享内存头部暴露统计信息

## 11. 扩展性设计

### 11.1 可扩展点
1. **新的数据类型**：在`PointType`枚举中添加新类型
2. **新的质量标记**：在`Quality`枚举中添加新标记
3. **新的单位**：在`Unit`枚举中添加新单位
4. **新的平台支持**：实现新的OSAL平台适配
5. **新的通知机制**：实现新的`INotification`接口

### 11.2 插件架构
- **配置插件**：支持YAML/JSON/XML配置格式
- **监控插件**：支持Prometheus/Graphite监控
- **桥接插件**：OPC UA/MQTT/Modbus桥接

## 12. 性能目标

### 12.1 关键性能指标

| 指标 | 要求 | 测试条件 |
|------|------|----------|
| 写入延迟（P99） | ≤ 10 μs | ARM Cortex-A53, 10k点位 |
| 读取延迟（P99） | ≤ 5 μs | 同上 |
| 写入吞吐量 | ≥ 50k 点/秒 | 多线程并发 |
| 内存占用 | ≤ 80 MB（10k点位） | 包含头部+点位数据 |
| 启动时间 | ≤ 100 ms | 冷启动 |

### 12.2 优化策略
- **内存对齐**：避免缓存伪共享
- **零拷贝读取**：直接返回共享内存指针
- **无锁设计**：读操作完全无锁
- **批量操作**：支持批量读写操作
- **预取优化**：数据预取减少缓存缺失

## 13. 风险评估与缓解

### 13.1 技术风险

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| Seqlock在高写冲突下性能下降 | 低 | 中 | 工业场景"多读少写"，冲突概率<0.1% |
| 跨平台兼容性问题 | 中 | 高 | OSAL层封装，单元测试覆盖所有平台 |
| 内存对齐导致的性能问题 | 低 | 中 | 严格对齐检查，性能测试验证 |
| 共享内存泄漏 | 低 | 高 | RAII封装，valgrind内存检查 |

### 13.2 项目风险
- **开发进度风险**：采用迭代开发，每2-4周一个迭代
- **质量风险**：严格代码审查，测试覆盖率≥90%
- **文档风险**：代码与文档同步更新，自动化文档生成

## 14. 下一步计划

### 14.1 短期计划（1-2周）
1. 完成核心层完整实现
2. 实现Linux平台OSAL
3. 完成基础单元测试
4. 创建基本使用示例

### 14.2 中期计划（3-4周）
1. 实现SylixOS平台适配
2. 完成集成测试
3. 性能测试与优化
4. 创建高级使用示例

### 14.3 长期计划（5-8周）
1. 实现配置管理（YAML）
2. 实现监控指标导出
3. 创建OPC UA桥接模块
4. 文档完善与发布

## 15. 结论

InduRTDB工程框架设计严格遵循工业级实时系统的要求，具备以下特点：

1. **高性能**：P99读写延迟≤10μs，满足工业控制需求
2. **高可靠**：7×24小时稳定运行，无内存泄漏
3. **易集成**：提供C++/C API，支持多平台
4. **可扩展**：模块化设计，支持功能扩展
5. **可维护**：清晰的分层架构，严格的编码规范

本设计为InduRTDB的开发提供了完整的蓝图，确保项目能够按时、高质量地完成。

---
**文档版本历史**
- v1.0.0 (2026-03-27)：初始版本，完成整体架构设计
- v1.1.0 (2026-05-11)：Core 层去虚函数/去 STL，PointManager 改为模板接口，Seqlock 回归轻量自由函数

**审核记录**
- 设计评审：v1.1.0 已修订
- 代码评审：待完成
- 测试评审：待完成