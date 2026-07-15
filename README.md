# InduRTDB — Industrial Real-Time Database

**版本 2.1.0** | 2026-05-11

## 项目概述

InduRTDB 是面向工业边缘控制场景（BAS、DDC、PLC）的**超低延迟、多进程共享、语义完备**的实时数据库。其设计严格遵循工业级实时系统开发规范。

> "让工业边缘开发者像访问全局变量一样简单、安全、高效地使用实时数据。"

## 核心特性

| 特性 | 状态 | 说明 |
|------|------|------|
| **超低延迟** | ✅ 已实现 | P99 ≤10μs (目标), 全局 Seqlock 无锁读 |
| **工业语义** | ✅ 已实现 | 每个点位携带 quality/unit/access/timestamp |
| **多进程共享** | ✅ 已实现 | POSIX shm_open + mmap(MAP_SHARED) |
| **多进程读写** | ✅ 已验证 | 59 单元/集成测试通过, 含 6 个 fork 多进程测试 |
| **C++ API** | ✅ 已实现 | `write<T>()` / `read()` / `peek()` / `subscribe()` / `loadConfig()` |
| **C ABI** | ✅ 已实现 | `indurtdb_write_*` / `indurtdb_read_*` 等 17 个函数 |
| **YAML 配置** | ✅ 已实现 | 轻量解析器, 零第三方依赖 |
| **零 STL / 零异常** | ✅ 严格遵循 | Core 层: 定长数组, 非虚, `-fno-exceptions -fno-rtti` |
| **SylixOS 支持** | ⚠️ OSAL 就绪 | 接口存在, 待平台验证 |
| **性能验证** | ⏳ 待完成 | ARM Cortex-A53 目标硬件 P99 测量 |

## 架构设计

```
┌───────────────────────────────────────────────────────┐
│                Application Layer                       │
│  • BAS 驱动 / 控制逻辑 / HMI / OPC UA Bridge           │
├───────────────────────────────────────────────────────┤
│                   API Layer                            │
│  • InduRTDB C++ 类 (单例, PIMPL, write<T>)            │
│  • C ABI (17 函数, 兼容 C/Python/Rust)                 │
├───────────────────────────────────────────────────────┤
│                   Core Layer (平台无关)                 │
│  • PointManager      — 直操共享内存, Seqlock 保护写    │
│  • SubscriptionMgr   — 定长 SubscriberSlot[256]        │
│  • SharedMemorySegment — shm/mmap/Header 初始化         │
│  • ConfigLoader      — 轻量 YAML 解析器                │
│  • Seqlock (自由函数) — seqlock_write_begin/end/read   │
├───────────────────────────────────────────────────────┤
│                OS Abstraction Layer (OSAL)             │
│  • ISharedMemory / ITime / IThreading / INotification  │
│  • POSIX 实现 (Linux)                                  │
│  • SylixOS 实现 (待移植)                                │
└───────────────────────────────────────────────────────┘
```

### 核心设计原则

1. **确定性优先** — 性能可预测，执行时间确定
2. **可靠性至上** — 7×24 小时稳定运行，无内存泄漏
3. **简洁性核心** — 代码简洁明了，避免过度设计
4. **平台无关性** — Core 层 100% 平台无关，OSAL 层隔离平台差异

## 编码规范约束

- **禁止异常**: `-fno-exceptions -fno-rtti`
- **禁止 STL 容器**: Core 层定长数组替代 `vector`/`map`
- **Core 层禁止虚函数**: 编译期绑定
- **共享内存 POD only**: 禁止指针、虚表、引用
- **返回码而非异常**: 全部使用 `bool`

## 项目结构

```
InduRTDB/
├── include/indurtdb/
│   ├── api/         # InduRTDB C++ API + C ABI 头
│   ├── core/        # PointManager, SubscriptionManager,
│   │                #   Seqlock, SharedMemorySegment,
│   │                #   ConfigLoader
│   ├── osal/        # ISharedMemory, ITime, IThreading,
│   │                #   INotification 接口 + Factory
│   ├── types/       # PointData, InduRTDBHeader, 枚举
│   └── utils/       # alignment, error, logging
├── src/
│   ├── api/cpp/     # indurtdb_impl.cpp
│   ├── api/c/       # C ABI 桥接
│   ├── core/        # point_manager, shared_memory_segment,
│   │                #   subscription_manager, config_loader,
│   │                #   seqlock
│   ├── osal/posix/  # POSIX 实现 (Linux)
│   ├── osal/sylixos/# SylixOS 实现
│   └── utils/       # alignment, error, logging
├── tests/
│   ├── unit/        # 53 单元测试 (7 test suites)
│   └── integration/ # 6 多进程集成测试
├── docs/            # 需求/设计/技术 文档
├── examples/        # 使用示例
├── VERSION          # 2.1.0
├── CHANGELOG.md
├── CMakeLists.txt
├── LICENSE (MIT)
└── README.md
```

## 构建与测试

### 构建要求

- 编译器：GCC ≥7.5 (C++17)
- 构建系统：CMake ≥3.15
- 目标平台：Linux (glibc), SylixOS
- 目标硬件：ARM Cortex-A53/A72, ≥64MB RAM

### 构建命令

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(nproc)
```

### 测试结果

```
[==========] 59 tests from 8 test suites ran.
[  PASSED  ] 59 tests.
```

| 测试套件 | 用例数 | 说明 |
|---------|--------|------|
| MultiProcessTest | 6 | 多进程共享内存集成测试 |
| SeqlockTest | 7 | Seqlock 无锁读写算法 |
| SubscriptionManagerTest | 10 | 订阅管理器 (定长数组) |
| MemoryLayoutTest | 11 | 共享内存布局 |
| BasicTypesTest | 8 | 基础类型 |
| AlignmentTest | 11 | 内存对齐工具 |
| ErrorTest | 3 | 错误处理 |
| LoggingTest | 3 | 日志系统 |

## C++ API 快速开始

```cpp
#include <indurtdb.hpp>

int main() {
    auto& rtdb = indurtdb::InduRTDB::instance();
    rtdb.initialize("hvac_system", 10000, 32);

    // 写入
    rtdb.write(1001, 23.5);          // double  (温度)
    rtdb.write(2001, (int32_t)42);   // int32   (计数器)
    rtdb.write(3001, true);          // bool    (开关)
    rtdb.write(4001, "Pump_01");     // string  (设备名)

    // 读取
    indurtdb::PointData p;
    if (rtdb.read(1001, p)) {
        printf("温度: %.1f, 质量: %d\n", p.value.d, p.quality);
    }

    // 零拷贝 peek
    const auto* pp = rtdb.peek(1001);

    rtdb.shutdown();
}
```

## C API 快速开始

```c
#include <indurtdb/api/c/indurtdb_c.h>

int main() {
    indurtdb_initialize("hvac_system", 10000, 32);
    indurtdb_write_double(1001, 23.5);

    indurtdb_point_t p;
    indurtdb_read_point(1001, &p);
    printf("温度: %f\n", p.value.d);

    indurtdb_shutdown();
}
```

## 性能指标 (目标)

| 指标 | 要求 | 测试条件 |
|------|------|----------|
| 写入延迟 (P99) | ≤ 10 μs | ARM Cortex-A53, 10k 点位 |
| 读取延迟 (P99) | ≤ 5 μs | 同上 |
| 写入吞吐量 | ≥ 50k 点/秒 | 多线程并发 |
| 内存占用 | ≤ 80 MB (10k 点位) | 含 Header + Points |
| 启动时间 | ≤ 100 ms | 冷启动 |

## 文档索引

| 文档 | 位置 | 版本 |
|------|------|------|
| 需求规格说明书 (SRS) | `docs/需求文档/` | v2.1.0 |
| 编码规范 | `docs/需求文档/编码规范.md` | v2.1.0 |
| 概要设计 (HLD) | `docs/设计文档/` | v2.1.0 |
| 详细设计 (LLD) | `docs/设计文档/` | v2.1.0 |
| Seqlock 算法设计 | `docs/技术文档/` | v2.1.0 |
| 项目开发总结 | `docs/开发规划/` | v2.1.0 |
| 变更日志 | `CHANGELOG.md` | — |

## 许可证

MIT License — 详见 `LICENSE` 文件。

---

**InduRTDB 不是"另一个数据库"，而是工业边缘控制系统的"神经系统"。**
它的存在，是为了让确定性、可靠性和简洁性回归工业软件的本质。
