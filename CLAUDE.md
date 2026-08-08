# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

InduRTDB（Industrial Real-Time Database，版本 2.1.0）是面向工业边缘控制场景（BAS、DDC、PLC）的**超低延迟、多进程共享、语义完备**的实时数据库。核心卖点：P99 ≤10μs 无锁读、每个点位携带 quality/unit/access/timestamp 工业语义、POSIX shm 多进程共享、零 STL / 零异常 / 零动态内存。

本仓库位于 edge-framework 的 `src/comms/InduRTDB/`，但**是独立 git 仓库**（origin: github.com:turnarond/InduRTDB），不受 edge-framework 构建系统管理。未来方向见远端分支 `feature/pure-c-rewrite`、`feature/v3.1.0`、`develop`。

## 工作约定（强制）

1. **交流使用中文**，专有名词（API 名、类名、技术名词）除外。代码注释、提交信息、文档均为中文。
2. **文档是唯一对外接口**：`docs/` 下的需求/设计/技术文档必须保持最新，代码与文档不允许出现分歧（防止文档腐败）。修改功能/API/架构时，必须同步更新对应文档。
3. **文档命名**：`docs/` 下文件夹名、文件名必须使用中文，且用数字前缀排序（如 `01-白皮书`、`02-需求分析`、`03-设计`、`04-使用手册`、`05-部署文档`）。当前 `docs/` 已按「需求文档/设计文档/技术文档/开发规划/sdk」组织。
4. **分支策略**：每次需求开发、需求变更、修改 issue，必须新建分支（`feature/xxx` 或 `fix/xxx`），禁止直接在 main 上开发。
5. **工具集**：开发过程中按工程需要创建配套工具（脚本、静态检查、CI 等），沉淀到仓库。
6. **TDD 强制**：永远遵循红-绿-重构循环。每次请求实现，必须附带对应的测试代码，或指明要让它变绿的测试用例。
7. **代码整洁**：遵循《代码整洁之道》（Clean Code）。
8. **架构整洁**：遵循《架构整洁之道》（Clean Architecture）——分层清晰、依赖规则单向、平台无关。
9. **问题记录**：开发中发现但当下无法修复的问题，要么记录到记忆，要么直接提 issue 到仓库。

## 构建

```bash
# 要求: GCC ≥7.5 (C++17), CMake ≥3.15, 目标平台 Linux / SylixOS
cd build   # 或 mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug   # 默认 Release
make -j$(nproc)
```

- 产物: `build/libindurtdb.a`（静态库）+ 每个 `tests/*/test_xxx.cpp` 各自的可执行文件
- 编译标志: `-Wall -Wextra -Werror -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables`（零警告是硬要求）
- `BUILD_EXAMPLES` 默认 OFF，开启需 `cmake .. -DBUILD_EXAMPLES=ON`（examples 链接 `indurtdb` 库）
- 静态检查（未配置进 CMake）: `clang-tidy` / `cppcheck` / `clang-format`，见 `docs/需求文档/编码规范.md` 附录

## 测试

```bash
cd build
ctest --output-on-failure                 # 全部测试 (12 个 target, 79 用例 / 13 套件)
ctest -R test_point_manager               # 单个 target
./tests/unit/test_point_manager           # 直接运行某个测试
./tests/unit/test_seqlock --gtest_filter=SeqlockTest.*   # 单个套件
```

- 框架: Google Test（`GTest::gtest_main`，测试文件无独立 main）；每个 `tests/*/test_xxx.cpp` 独立 target 并经 `add_test` 注册，支持 `make test_xxx` 单独构建
- 关键套件: `MultiProcessTest`（6 个 fork 多进程集成测试, 子进程用 `_exit()` 安全退出避免误调 `shm_unlink`）、`MemoryLayoutTest`（共享内存布局 `static_assert` 校验）、`SeqlockTest`、`PointManagerTest`、`SharedMemorySegmentTest`、`ConfigLoaderTest`
- 编码规范要求"每个 .cpp 有对应 test_xxx.cpp"，核心组件均已有直接单元测试；测试时间辅助用 `tests/unit/fake_time.hpp`（可控 `osal::ITime`）
- `-race` 类竞态检测对共享内存场景意义有限，多进程正确性靠 `MultiProcessTest` 验证

## 架构

```
API Layer    indurtdb::InduRTDB (单例 + PIMPL) ←→ C ABI (17 函数, indurtdb_c.h)
Core Layer   (平台无关, 零 STL / 零异常 / 非虚)
  PointManager            — 直操共享内存 PointData* 数组, 全局 Seqlock 保护写
  SubscriptionManager     — 定长 SubscriberSlot[256], C 函数指针回调, 心跳表清理
  SharedMemorySegment     — shm_open + ftruncate + mmap(MAP_SHARED), 创建者/使用者区分
  ConfigLoader            — 自研轻量 YAML 解析器 (零第三方依赖)
  Seqlock (自由函数)      — seqlock_write_begin/end/read, 无锁读
OSAL Layer   (接口 + Factory: ISharedMemory/ITime/IThreading/INotification)
  posix/   — Linux 实现 (当前唯一)
  sylixos/ — 接口就绪, 待平台验证
```

**共享内存布局**（POD only，`static_assert` 校验）: `InduRTDBHeader` + `PointData[max_points]` + `SubscriberEntry[max_subscribers]`。写路径持全局 `header_->write_seq` Seqlock；读路径无锁（`read()` 拷贝 / `peek()` 零拷贝返回共享内存指针）。`PointData` 128 字节定长，含 `value`（union: bool/int32/double/char[64]）、`type`、`quality`、`timestamp_ns`。

**核心设计原则**: 确定性优先、可靠性至上、简洁性核心、平台无关性。

## 编码红线（`docs/需求文档/编码规范.md`，代码审查直接拒绝）

- ❌ `std::vector` / `std::string` / `std::shared_ptr`（用定长数组 + `char[]` + 返回码替代）
- ❌ `new`/`delete`、`malloc`/`free`、异常、RTTI
- ❌ 共享内存中存指针 / 虚表 / 引用（必须 POD + `static_assert` 验证布局）
- ❌ `using namespace std;`、`printf`/`cout`（用日志宏）、未处理系统调用返回值（如 `shm_open`）
- ✅ 命名: 类 `UpperCamelCase`、函数 `lower_snake_case`、成员 `trailing_underscore_`、常量 `UPPER_SNAKE_CASE`（禁匈牙利命名）
- ✅ Core 层禁止虚函数（编译期绑定）；Doxygen 注释覆盖 public API
- ✅ 每个 `.cpp` ≤ 500 行，单一职责

## 构建结构注意点

- **新增测试**: 在 `tests/unit/`（或 `tests/integration/`）放 `test_xxx.cpp`，并把 target 名追加进该目录 `CMakeLists.txt` 的 `UNIT_TEST_SOURCES` 列表（unit 用 foreach 生成 target）；`tests/performance/` 仍是空壳 TODO
- 版本宏（`include/indurtdb.hpp`）必须与 `CMakeLists.txt` `project(VERSION)`、`VERSION` 文件一致，`VersionTest` 会校验；改版本时三处同步
- 文档索引: 产品白皮书在 `docs/01-白皮书/`，需求 SRS / 编码规范在 `docs/需求文档/`，概要/详细设计在 `docs/设计文档/`，Seqlock 算法在 `docs/技术文档/`，SDK 手册在 `docs/sdk/`（开发者指南/快速入门/C API/C++ API）
