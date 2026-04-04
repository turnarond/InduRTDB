# InduRTDB - Industrial Real-Time Database

## 项目概述

InduRTDB是一个面向工业边缘控制场景（如BAS、DDC、PLC）的**超低延迟、多进程共享、语义完备**的实时数据库。其设计严格遵循工业级实时系统开发规范，确保性能可预测、可靠性高、代码简洁。

## 核心特性

- ✅ **超低延迟**：P99读写延迟≤10μs，P99读取延迟≤5μs
- ✅ **工业语义**：每个点位携带质量、单位、权限、时间戳
- ✅ **多进程安全**：支持驱动、逻辑引擎、HMI并发访问
- ✅ **轻量可靠**：静态库≤50KB，7×24小时稳定运行无故障
- ✅ **易集成**：提供C++/C API，可作为OPC UA Server后端

## 架构设计

### 四层架构

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

### 核心设计原则

1. **确定性优先**：性能可预测，执行时间确定
2. **可靠性至上**：7×24小时稳定运行，无内存泄漏
3. **简洁性核心**：代码简洁明了，避免过度设计
4. **平台无关性**：核心层100%平台无关，OSAL层隔离平台差异

## 技术规范

### 语言特性约束

- **C++17核心特性**：允许`auto`、`constexpr`、`std::optional`（仅头文件）
- **禁止异常**：`-fno-exceptions`编译选项
- **禁止RTTI**：`-fno-rtti`编译选项
- **禁止动态内存分配**：禁止`new`/`delete`、`malloc`/`free`、STL容器
- **限制虚函数使用**：仅在OSAL接口层允许
- **允许Lambda**：仅用于回调注册，不能捕获`this`跨线程

### 内存管理约束

- **零动态内存分配**：所有内存必须静态分配或使用共享内存
- **共享内存要求**：必须是POD类型，禁止包含指针、虚表、引用
- **内存对齐**：共享内存结构体必须显式对齐（如`aligned(64)`）
- **RAII封装**：文件描述符、socket等资源必须使用RAII包装

### 并发与线程安全

- **原子操作**：使用`__atomic_*`内建函数或`std::atomic`（仅简单load/store）
- **读路径无锁**：使用Seqlock实现无锁读取
- **线程亲和性**：控制逻辑线程应绑定到特定CPU核

### 错误处理

- **返回码而非异常**：全部使用返回码（`bool`或`enum class Status`）
- **日志规范**：使用轻量级日志宏，发布版可关闭

### 命名规范

- **类/结构体**：`UpperCamelCase`
- **函数/方法**：`lower_snake_case`
- **变量**：`lower_snake_case`
- **常量**：`UPPER_SNAKE_CASE`
- **成员变量**：`trailing_underscore_`

## 项目结构

```
indurtdb/
├── include/                    # 公共头文件
│   ├── indurtdb/              # 主命名空间
│   │   ├── api/               # API接口
│   │   ├── core/              # 核心接口
│   │   ├── osal/              # OS抽象接口
│   │   ├── types/             # 类型定义
│   │   └── utils/             # 工具接口
│   └── indurtdb.hpp           # 主包含文件
├── src/                       # 源代码
│   ├── api/                   # API实现层
│   ├── core/                  # 核心层
│   ├── osal/                  # OS抽象层
│   └── utils/                 # 工具层
├── tests/                     # 测试代码
│   ├── unit/                  # 单元测试
│   ├── integration/           # 集成测试
│   └── performance/           # 性能测试
├── examples/                  # 示例代码
└── docs/                      # 文档
```

## 构建与部署

### 构建要求

- **编译器**：GCC≥7.5或兼容C++17编译器
- **构建系统**：CMake≥3.15
- **目标平台**：Linux (glibc)、SylixOS
- **目标硬件**：ARM Cortex-A53/A72，≥64MB RAM

### 构建命令

```bash
# 克隆仓库
git clone https://github.com/yourorg/indurtdb.git
cd indurtdb

# 创建构建目录
mkdir build && cd build

# 配置CMake
cmake .. -DBUILD_TESTS=ON

# 构建项目
make -j$(nproc)

# 运行测试
ctest --output-on-failure

# 安装（可选）
sudo make install
```

## 基本使用示例

### C++ API

```cpp
#include <indurtdb.hpp>
#include <iostream>

int main() {
    auto& rtdb = indurtdb::InduRTDB::instance();
    
    if (!rtdb.initialize("hvac_system")) {
        std::cerr << "初始化失败" << std::endl;
        return 1;
    }
    
    rtdb.write(1001, 23.5);
    
    indurtdb::PointData point;
    if (rtdb.read(1001, point)) {
        std::cout << "温度: " << point.value.d << " °C" << std::endl;
        std::cout << "质量: " << static_cast<int>(point.quality) << std::endl;
    }
    
    rtdb.subscribe(2001, [](const indurtdb::PointData& p) {
        if (p.value.b) {
            std::cout << "水泵启动命令" << std::endl;
        }
    });
    
    rtdb.update_heartbeat();
    rtdb.shutdown();
    
    return 0;
}
```

### C API

```c
#include <indurtdb/api/c/indurtdb_c.h>
#include <stdio.h>

void pump_callback(uint32_t id, const indurtdb_point_t* point) {
    if (point->value.b) {
        printf("水泵启动命令 (点位 %u)\n", id);
    }
}

int main() {
    if (!indurtdb_initialize("hvac_system", 10000, 32)) {
        fprintf(stderr, "初始化失败\n");
        return 1;
    }
    
    indurtdb_write_double(1001, 23.5);
    
    indurtdb_point_t point;
    if (indurtdb_read(1001, &point)) {
        printf("温度: %f °C\n", point.value.d);
        printf("质量: %u\n", point.quality);
    }
    
    indurtdb_subscribe(2001, pump_callback);
    indurtdb_update_heartbeat();
    indurtdb_shutdown();
    
    return 0;
}
```

## 性能指标

| 指标        | 要求             | 测试条件                  |
| --------- | -------------- | --------------------- |
| 写入延迟（P99） | ≤ 10 μs        | ARM Cortex-A53, 10k点位 |
| 读取延迟（P99） | ≤ 5 μs         | 同上                    |
| 写入吞吐量     | ≥ 50k 点/秒      | 多线程并发                 |
| 内存占用      | ≤ 80 MB（10k点位） | 包含头部+点位数据             |
| 启动时间      | ≤ 100 ms       | 冷启动                   |

## 代码质量要求

### 测试覆盖率

- **单元测试**：≥90%函数覆盖率（Google Test + gcov）
- **并发测试**：必须包含
- **崩溃恢复测试**：必须包含（kill -9模拟进程崩溃）
- **性能测试**：P99≤10μs（perf / cyclictest）

### 编码规范检查

```bash
# 运行clang-format检查
clang-format -style=file -i include/ src/ tests/ examples/

# 运行静态分析
cppcheck --enable=all --inconclusive include/ src/

# 运行Valgrind内存检查
valgrind --leak-check=yes --track-origins=yes ./build/tests/unit/test_basic_types
```

## 许可证

MIT License - 详见LICENSE文件

## 相关文档

- **详细需求规格说明书**：`docs/需求文档/编码规范.md`
- **整体架构设计**：`docs/设计文档/InduRTDB 工程框架架构设计文档.md`
- **详细设计**：待补充
- **API参考**：待补充

## 联系方式

如有问题或建议，请通过以下方式联系：

- 提交Issue：[https://github.com/turnarond/indurtdb/issues](https://github.com/yourorg/indurtdb/issues)
- 发送邮件：\[[yanchaodong@outlook.com](mailto:maintainer@email.com)]

***

**InduRTDB不是“另一个数据库”，而是工业边缘控制系统的“神经系统”。**\
它的存在，是为了让确定性、可靠性和简洁性回归工业软件的本质。
