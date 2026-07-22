# InduRTDB — Industrial Real-Time Database

**版本 3.0.0** | 2026-07-21 | **更新**: 纯 C11 实现，无 C++ API

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
| **C API** | ✅ 已实现 | 纯 C11, 单头文件 `<indurtdb/indurtdb.h>`, 24 个函数 |
| **C++ 兼容** | ✅ 已实现 | `extern "C"` 包裹, 可直接在 C++ 中调用 |
| **零 STL / 零虚表** | ✅ 严格遵循 | Core 层: 定长数组, 非虚, 零堆分配 |
| **SylixOS 支持** | ⚠️ OSAL 就绪 | 接口存在, 待平台验证 |
| **性能验证** | ⏳ 待完成 | ARM Cortex-A53 目标硬件 P99 测量 |

## 架构设计

```
┌───────────────────────────────────────────────────────┐
│                Application Layer                       │
│  • BAS 驱动 / 控制逻辑 / HMI / OPC UA Bridge           │
├───────────────────────────────────────────────────────┤
│                   API Layer                            │
│  • 纯 C11 API (indurtdb.h, 24 函数, 单例)             │
│  • extern "C" 包裹，可被 C++ 调用                      │
│  • 单头文件，零 C++ 依赖，兼容 C/Python/Rust           │
├───────────────────────────────────────────────────────┤
│                   Core Layer (平台无关, 纯 C11)         │
│  • irt_point_manager   — 直操共享内存, Seqlock 保护写  │
│  • irt_subscription    — 定长 Slot 数组, C 函数指针回调 │
│  • irt_shm             — shm_open/mmap, Header 初始化   │
│  • irt_config          — 轻量 key=value 解析器          │
│  • irt_seqlock         — 无锁读写, __atomic builtins   │
├───────────────────────────────────────────────────────┤
│                OS Abstraction Layer (OSAL, 纯 C11)     │
│  • irt_shm_os_map/unmap/is_owner (POSIX + SylixOS)    │
│  • irt_time_now_ns (clock_gettime)                    │
└───────────────────────────────────────────────────────┘
```

### 核心设计原则

1. **确定性优先** — 性能可预测，执行时间确定
2. **可靠性至上** — 7×24 小时稳定运行，无内存泄漏
3. **简洁性核心** — 代码简洁明了，避免过度设计
4. **平台无关性** — Core 层 100% 平台无关，OSAL 层隔离平台差异

## 编码规范约束

- **纯 C11**: 库代码使用 C11 标准, `-Wall -Wextra -Werror`
- **共享内存 POD only**: 禁止指针、虚表、引用
- **返回码而非异常**: 全部使用 `int` 返回码 (0 = 成功)
- **定长数组**: 替代 STL vector/map, 编译期确定边界
- **__atomic builtins**: C11 原子操作, 保证多进程可见性

## 项目结构

```
InduRTDB/
├── include/indurtdb/
│   └── indurtdb.h          # 唯一公共头文件 (纯 C API, 24 函数)
├── src/
│   ├── api/
│   │   └── indurtdb.c      # 公共 API 实现 (单例串联所有模块)
│   ├── core/
│   │   ├── irt_shm.c/h     # 共享内存段管理
│   │   ├── irt_point_manager.c/h  # 点位读写 (Seqlock 保护)
│   │   ├── irt_subscription.c/h   # 订阅/通知/心跳
│   │   └── irt_config.c/h  # key=value 配置解析器
│   ├── internal/
│   │   ├── irt_types.h     # 共享内存布局 (Header/Point/Subscriber)
│   │   └── irt_seqlock.h   # Seqlock 无锁读写 (inline 自由函数)
│   └── osal/
│       ├── irt_osal.h      # OS 抽象接口
│       ├── posix/           # Linux POSIX 实现
│       └── sylixos/         # SylixOS 实现
├── tests/
│   ├── unit/               # 7 单元测试 (C API/config/layout/osl/pm/shm/sub)
│   └── integration/        # 1 集成测试 (多进程 + 布局回归, 3 用例)
├── docs/                   # 需求/设计/技术 文档
├── CHANGELOG.md
├── CMakeLists.txt
├── LICENSE (MIT)
└── README.md
```

## 构建与测试

### 构建要求

- 编译器: GCC >= 7.5 (C11 编译库, C++17 编译测试)
- 构建系统: CMake >= 3.15
- 目标平台: Linux (glibc), SylixOS
- 目标硬件: ARM Cortex-A53/A72, >=64MB RAM

### 构建命令

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 测试

```bash
cd build && ctest --output-on-failure
```

### 测试结果

```
100% tests passed, 0 tests failed out of 8
```

| 测试 | 类型 | 说明 |
|------|------|------|
| test_c_api | 单元 | 公共 API 全功能验证 (初始化/读写/范围/订阅) |
| test_c_config | 单元 | 配置加载器 (key=value 解析, 默认值) |
| test_c_layout_seqlock | 单元 | 共享内存布局 + Seqlock 算法 |
| test_c_osal | 单元 | OS 抽象层 (共享内存映射, 时间) |
| test_c_pm | 单元 | 点位管理器 (类型化写入, Seqlock 读, peek) |
| test_c_shm | 单元 | 共享内存段 (owner 检测, magic/version 校验) |
| test_c_sub | 单元 | 订阅管理器 (注册/通知/心跳/僵尸清理) |
| test_c_multi_process | 集成 | 多进程 fork + 布局回归 (3 用例) |

## C API 快速开始

```c
#include <indurtdb/indurtdb.h>

int main() {
    if (indurtdb_initialize("hvac_system", 10000, 32) != 0)
        return 1;

    // 写入
    indurtdb_write_double(1001, 23.5);      // double  (温度)
    indurtdb_write_int32(2001, 42);         // int32   (计数器)
    indurtdb_write_bool(3001, true);        // bool    (开关)
    indurtdb_write_string(4001, "Pump_01"); // string  (设备名)

    // 读取
    double temp;
    if (indurtdb_read_double(1001, &temp) == 0) {
        printf("温度: %.1f\n", temp);
    }

    // 零拷贝 peek
    const indurtdb_point_t* p = indurtdb_peek(1001);

    indurtdb_shutdown();
    return 0;
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
| 需求规格说明书 (SRS) | `docs/需求文档/` | v3.0.0 |
| 编码规范 | `docs/需求文档/编码规范.md` | v3.0.0 |
| 概要设计 (HLD) | `docs/设计文档/` | v3.0.0 |
| 详细设计 (LLD) | `docs/设计文档/` | v3.0.0 |
| Seqlock 算法设计 | `docs/技术文档/` | v3.0.0 |
| C API 参考手册 | `docs/sdk/C API 参考手册.md` | v3.0.0 |
| 快速入门指南 | `docs/sdk/快速入门指南.md` | v3.0.0 |
| 开发者指南 | `docs/sdk/开发者指南.md` | v3.0.0 |
| 变更日志 | `CHANGELOG.md` | — |

## 许可证

MIT License — 详见 `LICENSE` 文件。

---

**InduRTDB 不是"另一个数据库"，而是工业边缘控制系统的"神经系统"。**
它的存在，是为了让确定性、可靠性和简洁性回归工业软件的本质。

v3.0.0 是 InduRTDB 的里程碑版本——从 C++ 到 C11 的完全重写。
仅保留一个公共头文件 (`indurtdb.h`)，零 C++ 依赖。
所有内部模块以 `irt_` 前缀命名，编译时 `-Wall -Wextra -Werror` 保证代码质量。
