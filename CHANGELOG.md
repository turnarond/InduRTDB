# Changelog

All notable changes to InduRTDB.

---

## [3.0.0] — 2026-07-21

### 概述
纯 C 重写。所有模块从 C++ (v2.x) 直译为 C11，保证共享内存布局逐字节一致。

### 收益
- **API 面积极小**：全部功能浓缩为单一头文件 `indurtdb.h`，仅 100 行、24 个函数，学习成本几乎为零
- **零 C++ 运行时依赖**：不依赖 STL、异常、RTTI、虚表，可在任何 C11 编译器上构建和链接
- **代码量大幅缩减**：核心 C 源码仅 ~760 行 + 内部头 ~300 行，远少于原 C++ 实现（含 C API 桥接层）
- **编译速度显著提升**：无模板展开、无 STL 头文件包含链，增量编译接近 C 编译速度
- **确定性执行**：去除虚函数调用（间接跳转），所有调用路径编译期确定，更利于 WCET 分析
- **跨语言 FFI 原生兼容**：纯 C API 可被 C++、Python ctypes、Rust FFI、Go cgo 直接调用，无需桥接层
- **SylixOS 友好**：避免 SylixOS 对 C++17 特性支持不完整的问题（特别是 stdatomic），全部使用 `__atomic` builtins
- **单例无锁设计**：全局 Seqlock + 无堆分配，运行时无内存碎片，适合 7x24 工业场景

### Added
- 纯 C 公共 API (`include/indurtdb/indurtdb.h`): 24 个函数，单头文件，零 C++ 依赖
- C11 OSAL 层 (`irt_osal.h`): POSIX + SylixOS 双平台，无虚表
- irt_shm 共享内存段管理: shm_open/mmap, owner 检测, magic/version 校验
- irt_point_manager: 4 种类型写入 (bool/int32/double/string), seqlock 读, 零拷贝 peek
- irt_subscription: 回调注册/通知/心跳/僵尸清理, 定长 Slot 数组
- irt_config: 轻量 key=value 解析器, zero-allocation
- 单元测试 x7 (C API, config, layout+seqlock, osal, point_manager, shm, subscription)
- 集成测试 x1 (多进程 fork + 布局回归): 3 用例, 覆盖父子进程读写和原始字节布局校验
- `indurtdb_peek()`: 零拷贝读取, 返回共享内存直接指针
- `indurtdb_read_range()` / `indurtdb_write_range_*()`: 批量读写接口
- `indurtdb_subscribe()` / `indurtdb_unsubscribe()`: 变更订阅
- `indurtdb_load_config()`: 从配置文件加载实例参数
- `indurtdb_update_heartbeat()` / `indurtdb_is_initialized()`: 心跳与状态查询

### Changed
- **语言**: C++17 → C11 (gcc), 测试保留 C++17+gtest
- **构建**: `-std=c++11` → `-std=c11` (库), `-std=c++17` (测试)
- **编译选项**: C 文件 `-Wall -Wextra -Werror`
- **头文件**: `<indurtdb/api/c/indurtdb_c.h>` → `<indurtdb/indurtdb.h>`
- **单例模式**: C++ static local + PIMPL → C static global struct
- **API 设计**: 模板 write<T>() → 显式类型函数 (write_bool/int32/double/string)
- **模块命名**: irt_ 前缀 (InduRTDB C 实现), 内部头不暴露给用户
- **测试框架**: 7 个独立 test suite → 统一 C API 测试套件
- **项目结构**: include/ 精简为单一公共头, src/ 按 core/api/osal/internal 组织

### Fixed
- 消除所有虚函数开销 (OSAL, PointManager, SubscriptionManager)
- 消除所有 STL 容器依赖 (vector/map/function/mutex/unique_ptr)
- 消除所有异常处理路径
- 修复 Seqlock 多进程 ABA 防护 (uint64_t 保证不溢出)

### Removed
- C++ PIMPL 实现层 (`src/api/cpp/`)
- C ABI 桥接层 (`src/api/c/`)
- `ISeqlock` / `ISharedMemory` / `ITime` / 所有虚接口
- `std::function` / `std::vector` / `std::unordered_map` / `std::mutex`
- POSIX C++ 封装 (`src/osal/posix/` C++ 文件)
- C++ 单元测试 (API/config/layout/seqlock/osal/pm/shm/sub — 全部以 C 重写)
- `SeqlockFactory` / `SubscriptionManagerUtils` 等工具类

### 兼容性
- 共享内存布局逐字节兼容 v2.x: Header (64B) + PointData (128B) + SubscriberEntry (16B)
- magic (0x1DBA1DBA), version (1) 保持不变
- C API 函数签名与 v2.x C ABI 向后兼容

---

## [2.1.0] — 2026-05-11

### Added
- 多进程集成测试 (6 用例)：AllDataTypes, ZeroCopyPeek, MultipleReaders, BulkMixedTypes, HeartbeatVisible, InstanceIsolation
- `safe_fork_and_run` 模式：子进程通过 `_exit()` 安全退出，避免继承的析构函数错误地 `shm_unlink`
- 轻量 YAML 配置解析器 (`ConfigLoader`)，零第三方依赖
- `InduRTDB::write(PointId, const char*)` 非模板重载，修复字符串字面量类型推导
- `SharedMemorySegment` 公共头文件 (`shared_memory_segment.hpp`)
- `VERSION` 文件

### Changed
- **Seqlock 重构**：200+ 行 OOP 层次（ISeqlock/SeqlockException/Factory/Utils）→ 70 行轻量 inline 自由函数 (`seqlock_write_begin/end/read`)
- **PointManager 重写**：虚接口 + `vector<unique_ptr<ISeqlock>>` → 非虚类 + 直接操作共享内存 `PointData*` 数组 + 全局 `header_->write_seq` Seqlock
- **SubscriptionManager 重构**：`std::unordered_map`/`std::vector`/`std::function`/`std::mutex` → 定长 `SubscriberSlot[256]` + C 函数指针回调
- **API 层**：去掉全局 `std::mutex`，`subscribe`/`loadConfig`/`updateHeartbeat` 从 stub 改为完整实现
- **C ABI**：17 个函数从 stub 改为完整桥接实现
- **OSAL**：删除重复的 `linux/` wrapper 层和废弃的 `interface/` stubs，统一为 POSIX 实现
- SharedMemorySegment 修复：`unique_ptr<ISharedMemory>` 生命周期从局部变量提升为成员变量
- PointManager::peek() 从 `static temp` 拷贝改为真正零拷贝（直接返回 `&points_[id]`）
- 构建：`libindurtdb.a` 编译通过，零警告；59 tests 全部通过

### Fixed
- Seqlock `s1` 可能未初始化 (`-Werror=maybe-uninitialized`)
- `SubscriptionManager` 测试缓冲区溢出（`shm_table_[4]` 传入 `max_subscribers=32`）
- `AlignmentTest` 两处断言逻辑错误（栈对齐假设 / `posix_memalign` 最小对齐要求）
- `write("string literal")` 类型推导为 `const char[N]` 导致的链接错误

### Removed
- `ISeqlock` / `Seqlock` / `SeqlockException` / `SeqlockFactory` / `SeqlockUtils` 类层次
- `ISubscriptionManager` 虚接口
- `src/osal/linux/` 目录（POSIX 重复封装）
- `src/osal/interface/` 目录（废弃 base 类）
- 各测试文件独立的 `main()` 函数（统一使用 `GTest::gtest_main`）

---

## [1.0.0] — 2026-03-27

### Added
- 初始项目脚手架
- 四层架构：Application / API / Core / OSAL
- 类型系统：`PointData` (128B), `InduRTDBHeader` (64B), `SubscriberEntry` (16B)
- OOP 风格 Seqlock 实现 (`ISeqlock` / `Seqlock` / `SeqlockFactory`)
- `PointManager` 虚接口 + `PointManagerImpl`
- `SubscriptionManager`（STL 容器实现）
- `SharedMemorySegment` 接口定义（空壳）
- `ConfigLoader` 接口定义（空壳）
- C++/C API stub
- POSIX OSAL 实现 (shm_open/mmap, Unix Domain Socket, pthread)
- Google Test 框架集成
- 需求文档 (SRS, 编码规范)
- 设计文档 (HLD, 工程框架架构, LLD)
- 技术文档 (Seqlock算法设计)
- 开发规划
