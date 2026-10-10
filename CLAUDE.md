# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# 语言规则
- 所有回答都使用简体中文。
- 代码注释、错误信息和提示都使用中文。
- 当涉及专有技术术语时，可以保留英文原文，但请提供中文解释。
- 代码本身（如变量名、函数名）、命令行指令、配置文件内容，则根据实际需要保持原样，不要翻译。

# 协作与工程流程规范

## 仓库卫生（Git 纪律）
- **不允许直接 commit 和 push**：任何提交/推送前先向用户确认；由用户决定时机与分支。
- 提交按**逻辑单元**合并，要精简，不要每一个小改动一个 commit（中间态、chore 类改动并入所属功能提交）。
- **禁止在 git 仓库中出现 AI / 插件相关的中间目录或产物**（如 `.claude/`、`.omc/`、`.cache/`、`docs/superpowers/`、SDD 过程残留等）；运行期本地目录必须加入 `.gitignore`。仓库根目录只保留 CLAUDE.md 与 AGENTS.md 两个 AI 辅助文件。
- **不允许直接在 main 上开发**。
- 版本在**打包节点同步打 tag**（标注版本信息），并同步更新 `VERSION` 与 `CHANGELOG.md`。

### 分支模型（release 分支制）

```
main                     ── 只接受已发布版本的合并，长期稳定
  └─ release/<版本号>      ── 版本分支，如 release/2.2.0，本地与远程都长期保留
       ├─ feature/xxx  ──┐
       └─ fix/xxx     ──┴── 合入 release 分支后即删除（本地 + 远程）
                          版本分支开发完成 → 打 tag <版本号>
```

- **每个版本对应一个版本分支** `release/<版本号>`，从 `main` 切出；版本分支本地与远程**都保留**，作为该版本的永久历史。
- **所有 feature / fix 分支一律合入所属的版本分支**，不得直接合入 `main`，也不得跨版本分支合入。
- **保持分支整洁**：feature / fix 分支合入版本分支并验证通过后，**本地与远程分支都要删除**，避免堆积。
- **版本分支开发完成时打 tag** `<版本号>`（如 `v2.2.0` 或 `2.2.0`，择一后全仓库统一），并同步 `VERSION` + `CHANGELOG.md`。tag 打在版本分支的最新提交上。
- 版本分支收尾后不再接受新特性，只接受 bugfix 与安全修复；确需追加功能时，另开新版本分支。

## 文档即接口
- 文档是产品的**唯一对外接口**：代码与文档必须同步更新，文档不允许滞后或腐败。
- `docs/` 下的文件夹、文件名一律使用**中文**，并按数字前缀排序，当前目录结构：
  - `01-白皮书` — 产品总览 / roadmap / 项目开发总结
  - `02-需求分析` — SRS、issue 汇总
  - `03-设计` — 概要设计 (HLD)、详细设计 (LLD)、算法专题、编码规范
  - `04-使用手册` — SDK 快速入门、开发者指南、C/C++ API 参考手册
  - `05-部署文档` 等后续目录按编号续排；新增文档放对应类别目录
- 产品要有总体白皮书/roadmap；每个版本要有明确的版本边界描述。

## 开发流程（SDD + TDD）
- 流程走 SDD：需求分析 → 方案设计 → 任务规划 → 实施 → 交付。**关键阶段必须有专家评审，尤其是方案设计**；方案获批前不动代码。
- 使用 **TDD**，永远遵循红-绿-重构循环：每次请求实现，必须附带对应的测试代码，或指明要让它变绿的测试用例；先见失败，再做最小实现。
- **重构要一起讨论**：当新增/修改功能与现有架构冲突较大、或实现别扭时，不硬改，先提出重构方案与用户讨论，批准后再动手。
- 代码整洁（代码整洁之道）与架构整洁（架构整洁之道）：分层单向依赖（见下），全局考虑，不留死代码与 TODO 占位。
- **开发中发现的问题**：当前没有时间改的记入 issue；**不属于本项目的问题只排查不解决**，通过 issue 提交给对应的工程/SDK/文档提供方。

## 质量与稳定性要求
- 分析、评审、测试时考虑**长久运行的稳定性**：内存泄漏、句柄/文件描述符泄漏、共享内存段残留、订阅表/点位表资源耗尽。
- 评审与验证手段：gtest 单测（-race 类竞态检查对应本工程用 TSAN/ASAN 构建）、valgrind、`ctest`。
- 调试：Linux 使用 gdb；Windows 使用 cdb，Windows 不好调可转 WSL。
- **SDK 前向兼容（本工程为 SDK 库）**：公开头文件/接口变更必须保证二进制与源码前向兼容，避免 ABI 不匹配导致使用者被迫用新 SDK 重编；新增能力优先用**追加新接口**，不修改既有签名、结构体布局或枚举取值。

## 配套工具
- 根据工程需要建设开发工具集：在 `tools/` 下用 Python 做成**系列化配套工具**，每个工具自成小型工程结构，用于测试、部署、冒烟等验证；随功能开发同步补齐。

## Build & Test

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON          # 构建示例加 -DBUILD_EXAMPLES=ON
make -j$(nproc)
./indurtdb_tests                   # 运行全部单元/集成测试（GTest）
./indurtdb_tests --gtest_filter='SeqlockTest.*'   # 运行单个测试组
ctest -R indurtdb_tests            # 经 CTest 运行
```

产物：`libindurtdb.a`（静态库）。
**Requirements**: GCC >= 7.5 (C++17)、CMake >= 3.15、Google Test（可选，用于测试）。

## Architecture

InduRTDB 是面向工业边缘控制的**多进程共享内存实时数据库**。4 层架构：

```
Application → API (C++ PIMPL singleton / C ABI) → Core (平台无关) → OSAL (OS 抽象)
```

**启动流程**（`src/api/cpp/indurtdb_impl.cpp` 的 `Impl::initialize`）：
1. `OSALFactory::create_time()` — 获取 OS 时间源
2. `SharedMemorySegment` — `shm_open` + `mmap(MAP_SHARED)`，首进程为 owner 并初始化 header（魔数 `0x1DBA1DBA`），后续进程为 joiner 仅校验
3. `PointManager(shm_base, max_points, time)` — 直接操作共享内存上的 `PointData*` 数组
4. `SubscriptionManager(time, subscriber_table, max_subscribers)` — 固定 `SubscriberSlot[256]`

**共享内存布局**（`include/indurtdb/types/memory_layout.hpp`，均有 POD 与定长 `static_assert`）：
`[InduRTDBHeader 64B] [PointData[max_points] 128B] [SubscriberEntry[max_subscribers] 16B]`

### 核心组件

| 组件 | 文件 | 职责 |
|------|------|------|
| `SharedMemorySegment` | `src/core/shared_memory_segment.cpp` | 管理 shm 生命周期（`shm_open`/`mmap`/`shm_unlink`） |
| `PointManager` | `include/indurtdb/core/point_manager_interface.hpp` | 非虚、零分配。`write<T>()` 取全局 Seqlock 后原位更新 `PointData`；`peek()` 返回共享内存直接指针（零拷贝）；`read()` 拷出 |
| `SubscriptionManager` | `include/indurtdb/core/subscription_manager_interface.hpp` | 固定 `SubscriberSlot[256]` 数组 + C 函数指针（`SubscriptionCallback`）；无 `std::function`/`std::mutex` |
| `ConfigLoader` | `include/indurtdb/core/config_loader.hpp` | 零依赖 YAML 解析器，用于点位配置文件 |
| **Seqlock** | `include/indurtdb/core/seqlock.hpp` | 3 个内联自由函数（`seqlock_write_begin`/`end`/`read`），作用于 `header_->write_seq` |

### OSAL（OS 抽象层）

虚接口在 `include/indurtdb/osal/interface.hpp`：`ISharedMemory`、`ITime`、`IThreading`、`INotification`；工厂在 `include/indurtdb/osal/factory.hpp`。POSIX 实现在 `src/osal/posix/`，SylixOS stub 在 `src/osal/sylixos/`。平台在 CMake 中自动检测，错误实现文件不参与编译。

### API 层

- **C++ API**（`include/indurtdb/api/indurtdb.hpp`）：`InduRTDB` 单例 + PIMPL。`write<T>()` 委托给 `Impl`，后者依次调用 `PointManager::write()` 与 `SubscriptionManager::notify()`。
- **C ABI**（`include/indurtdb/api/c/indurtdb_c.h`、`src/api/c/indurtdb_c_impl.cpp`）：17 个扁平 C 函数桥接到 C++ 单例。

## Coding Constraints（Strict）

由 `CMakeLists.txt` 编译选项与工程约定强制，违反将破坏构建或导致数据损坏：

- **`-fno-exceptions -fno-rtti`** — 无异常、无 RTTI，错误一律 `bool` 返回。
- **Core 层无 STL 容器** — 用定长数组（`SubscriberSlot[256]`、`char[64]`）替代 vector/map/string。
- **Core 层无虚函数** — 仅编译期绑定。
- **共享内存仅 POD** — 无指针、无虚表、无引用；每个 shm 结构都有 `static_assert(std::is_pod_v<T>)` 与 `static_assert(sizeof(T) == N)`。
- **Core 层零动态分配** — `new`/`malloc` 只允许出现在 OSAL 工厂与 API Impl。
- **成员命名 `trailing_underscore_`** — `header_`、`points_`、`max_points_` 等。
- **锁自由原语用 `__atomic_*` 内建**（非 `std::atomic`）— 见 Seqlock 实现。
- `<indurtdb.hpp>` 是唯一公开入口头文件，其余头文件由它级联包含。

## Key Patterns

- **Seqlock 写**：`seqlock_write_begin(&header->write_seq)` 使 seq 变奇数；若已为奇数说明有其他写者，返回 false（不自旋等待）。`seqlock_write_end` 将 seq 置为 `s0+2`（下一偶数）。**单写者假设**。
- **Seqlock 读**：自旋直到 `s0 == s1` 且 `s0` 为偶数，随后返回共享内存直接指针。零拷贝；调用方不得跨写边界持有指针。
- **PIMPL**：`InduRTDB` 头文件是薄代理，全部逻辑在 `.cpp` 中的 `InduRTDB::Impl`；模板方法在 `.cpp` 内显式特化。
- **字符串写入**：`write(PointId, const char*)` 是非模板重载，规避 `const char[N]` 推导问题；模板 `write<T>` 处理 `bool`、`int32_t`、`double`。
- **多进程安全**：测试子进程用 `_exit()`（而非 `exit()`），避免继承的析构函数调用 `shm_unlink`。
