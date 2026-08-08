# Changelog

All notable changes to InduRTDB.

---

## [Unreleased] — 2026-08-09（分支 feature/whitepaper-and-known-issues）

### Added
- 产品白皮书 `docs/01-白皮书/InduRTDB产品白皮书.md`（以用户使用体验为主线）
- 核心组件单元测试：`PointManagerTest` (7)、`SharedMemorySegmentTest` (6)、`ConfigLoaderTest` (4) + `ConfigLoaderTypeTest` (1)、`VersionTest` (2)；新增 `fake_time.hpp` 测试辅助
- 测试独立 target 化：每个 `test_xxx.cpp` 独立可执行并逐条注册 ctest，支持 `make test_xxx` / `ctest -R`

### Changed
- 版本宏统一为 2.1.0（头文件原为 2.0.0，与 CMake/README/VERSION 对齐）
- 测试总数 59 → 79 用例、8 → 13 套件（单元 73 / 集成 6）

### Fixed
- ConfigLoader：`parse_kv` 值前导空白未去除（`trim` 返回指针被丢弃），导致引号剥离与类型解析失效
- ConfigLoader：`"- id: N"` 列表项行中 `"- "` 后首个 key:value 被吞掉，id 恒为 0
- 清理 `tests/` 子目录引用不存在 target 的死代码 CMakeLists

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
