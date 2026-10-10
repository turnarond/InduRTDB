# Changelog

All notable changes to InduRTDB.

---

## [2.2.0] — 2026-05-18

### Added
- **访问控制**（SRS §4.3）：`PointManager::write()` 拒绝写入 `Access::READ_ONLY` 点位。
  语义约定：`access == 0`（未经 `load_config` 配置）视为可写，保持向后兼容。
- **超时标记**（SRS §7.2）：`PointManager::mark_timeout()` / `InduRTDB::mark_timeout()`
  / `indurtdb_mark_timeout()`，置 `Quality::TIMEOUT` 并累加 `stats.timeouts`。
  采用被动标记——阈值由驱动层判定，库不做后台巡检。
- **订阅者计数**：`InduRTDB::get_subscriber_count()` / `indurtdb_get_subscriber_count()`（SRS §7.2）。
- `InduRTDB::get_timeout_count()` / `PointManager` 超时计数访问路径。
- 单元测试 14 个（`AccessControlTest` 6 例、`PointManagerTimeoutTest` 8 例），总数 59 → 73。
- `tests/smoke/smoke_c_api.c` 补齐超时标记与订阅计数的 C ABI 冒烟覆盖。
- `AGENTS.md` —— 面向 OpenCode/AI 会话的工程事实与陷阱清单。

### Fixed
- `indurtdb_get_timeout_count()` 原为硬编码 `return 0;` 的空实现，现已接入真实计数。
- `stats.timeouts` 此前在全项目中**无任何递增点**，监控指标恒为 0。
- 文档与代码失真：SRS §5.1 的 `loadConfig()`/`updateHeartbeat()` 实为
  `load_config()`/`update_heartbeat()`；§3.2 的单参 lambda 订阅示例**无法编译**，
  已更正为 3 参 C 函数指针签名。
- 版本号三处漂移：`VERSION` / `CMakeLists.txt` 为 2.1.0，`include/indurtdb.hpp` 为 2.0.0，
  现统一为 2.2.0。
- README 将「P99 ≤10μs」标注为已实现，但 `tests/performance/` 为空且从未接入构建，
  已改标为「未实测」并加注警示。
- C ABI 函数清单：实际 15 个而文档声称 17 个，补齐 `mark_timeout` /
  `get_subscriber_count` 后达到 17 个，清单与头文件逐一对齐。

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
