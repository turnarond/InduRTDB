# AGENTS.md — InduRTDB

> 姊妹文件：`CLAUDE.md`（项目协作与 SDD 流程规范，中文）。本文件只记录**从代码/构建脚本实测得到、容易踩坑**的事实。
> 语言约定：**回答与代码注释、错误信息一律用简体中文**（见 `CLAUDE.md`）。

## 一分钟上手

```bash
./verify.sh                 # 唯一推荐入口：环境检查 → cmake 配置 → 全量编译 → ctest → 冒烟测试
./verify.sh --clean         # 先删 build/ 再验证（改了 CMakeLists 后必须用）
./verify.sh --no-tests      # 只编译
./verify.sh --smoke-only    # 只跑冒烟测试（需已编译过）
```

- `verify.sh` 强制 `-DCMAKE_BUILD_TYPE=Debug`，而 `CMakeLists.txt` 默认是 **Release** —— 两者行为不同，别混用。
- 产物：`build/libindurtdb.a`（静态库）、`build/indurtdb_tests`（GTest，59 用例）、`build/tests/smoke/smoke_*`（3 个免 GTest 可执行文件）。
- 示例需显式开启：`cmake -S . -B build -DBUILD_EXAMPLES=ON` → `build/examples/{basic_example,c_example,multi_process_example}`。
- 无 CI、无 lint 配置、无 `.clang-format`（`docs/03-设计/编码规范.md` 提到的 clang-format/clang-tidy 尚未落地，不要去找）。

## 构建系统陷阱（最容易浪费时间的部分）

1. **根 `CMakeLists.txt` 用 `file(GLOB_RECURSE)` 收集源码，没有 `CONFIGURE_DEPENDS`。**
   新增/重命名/删除任何 `.cpp` 后，`make` **不会**自动感知。必须重跑 `cmake -S . -B build`（或 `./verify.sh --clean`）才生效。
2. **大量 `CMakeLists.txt` 是死文件**，根 `CMakeLists.txt` 从不 `add_subdirectory()` 它们：
   `include/CMakeLists.txt`、`src/{,api,core,osal,osal/posix,utils}/CMakeLists.txt`、`tests/CMakeLists.txt`、`tests/unit/CMakeLists.txt`、`tests/integration/CMakeLists.txt`、`tests/performance/CMakeLists.txt`。
   - `tests/unit/CMakeLists.txt` 里引用了**并不存在的 target** `indurtdb_core` / `indurtdb_osal` —— 不要试图通过 `add_subdirectory(tests/unit)` 来“修复”，会直接构建失败。
   - `tests/performance/` 从未被构建。
   - 唯一真正生效的子目录是 `tests/smoke/` 和 `examples/`。
3. **编译选项走全局 `CMAKE_CXX_FLAGS`，不是 `target_compile_options`** —— 库、测试、示例**全部**继承 `-Wall -Wextra -Werror -fno-exceptions -fno-rtti`。
   任何新增警告都会让整个构建失败；不要用 `#pragma GCC diagnostic` 局部压制。
4. `file(GLOB_RECURSE SOURCES "src/*.cpp")` 之后按平台 `list(FILTER ... EXCLUDE)` 剔除 `sylixos/`（SylixOS 分支剔除的是 `linux/`，与实际目录名 `posix/` 不匹配 —— 交叉编译到 SylixOS 时这里会露馅）。
5. `include/CMakeLists.txt` 里定义的 `indurtdb_headers` INTERFACE target（会提供 `INDURTDB_VERSION_*` 宏）同样从未被创建，所以那套宏实际是死代码。

## 共享内存：真正的坑

- 段名固定为 `/indurtdb_<instance_id>`（`shared_memory_segment.cpp:24`），落在 `/dev/shm`。
- **owner 判定用的是 `fstat` 后看 `st_size == 0`，不是 `O_EXCL`**（`src/osal/posix/shared_memory_posix.cpp:31-38`）。
  后果：若 `/dev/shm` 里残留上次的段，新进程会**静默当成 joiner** 接入旧数据（magic/version 校验会通过）。
  若残留段比本次请求的 `max_points` 小，则不会 `ftruncate`，`mmap` 出来的区域越界访问会 **SIGBUS**。
  → 测试/示例崩溃过后，**先 `rm -f /dev/shm/indurtdb_*` 再跑**。
- **`fork()` 出的子进程必须用 `_exit()` 收尾，绝不能 `exit()` 或从 main 返回。**
  子进程会继承父进程的析构链，`SharedMemorySegment` 析构 → `shm_unlink` → 把父进程还在用的段删掉。
  参考实现：`tests/integration/test_multi_process.cpp` 的 `safe_fork_and_run()`。
- 共享内存布局就是**进程间 ABI**。`memory_layout.hpp` 里三个结构体用 `__attribute__((packed, aligned(N)))` + `static_assert(sizeof(...) == 64/128/16)` 锁死。
  改字段 = 改 ABI，已存在的段会与新进程不兼容。段格式版本在 `shared_memory_segment.cpp:16`（`VERSION = 1`），改布局必须同步 bump。

## Core 层约束（写代码前先看）

已实测成立：

- Core 层**零 `virtual`**、零 `std::mutex` / `std::function` / `std::thread` / `std::atomic`（原子一律用 `__atomic_*` 内建）。
- 订阅表是 `SubscriberSlot[256]` 定长数组，回调是 C 函数指针 `SubscriptionCallback`。
- 错误一律 `bool` / 返回码返回，**没有异常机制可依赖**。

`CLAUDE.md` 中**两处说法已过时/不准确**，别照抄：

- “Core 层零动态分配”并不成立：`core/config_loader.cpp` 用 `calloc/realloc/free`，`core/shared_memory_segment.hpp:53` 用 `std::unique_ptr<osal::ISharedMemory>`，`utils/alignment.cpp` 用 `malloc/free`。
  真正的约束是：**热路径（`PointManager::write/read/peek`）零分配**，且禁止 `vector/map/string` 等容器。
- `docs/03-设计/编码规范.md` §10 把 `printf/cout` 列为红线，但现网代码（`shared_memory_segment.cpp`、`indurtdb_impl.cpp`）用的是 `std::fprintf(stderr, ...)`。照抄代码现状，别按红线改写成日志宏。

## API 易错点

- `InduRTDB` 是**单例 + PIMPL**，不可拷贝。必须 `initialize()` → 使用 → `shutdown()`，所有方法在未初始化时返回 `false` / `nullptr`。
- `write<T>` 只支持 `bool` / `int32_t` / `double`，其他类型在**编译期** `static_assert` 失败。
- `write(PointId, const char*)` 是**非模板重载**，专为规避 `const char[N]` 字面量推导问题而存在 —— 加新写入类型时注意区分模板与非模板两条路径。
- `PointManager` / `SubscriptionManager` **不持有共享内存所有权**，构造时由 `SharedMemorySegment` 传入 mmap 基址；释放顺序由 `Impl` 负责（`sm_ → pm_ → seg_`）。
- `peek()` 返回的是**共享内存内的裸指针**，零拷贝。不得跨写边界持有该指针。
- Seqlock 遵循**单写者假设**：`seqlock_write_begin` 发现 seq 为奇数即直接返回 `false`，**不自旋等待**。并发写测试要按这个语义写。

## 测试

- 全量：`ctest --test-dir build --output-on-failure`（注册了 4 个测试：`indurtdb_tests` + 3 个 smoke）。
- 单个 suite：`./build/indurtdb_tests --gtest_filter='SeqlockTest.*'`。
  可用 suite 名：`MultiProcessTest` `AlignmentTest` `BasicTypesTest` `ErrorTest` `LoggingTest` `MemoryLayoutTest` `SeqlockTest` `SubscriptionManagerTest`（共 59 用例）。
- 单个用例：`--gtest_filter='MultiProcessTest.ZeroCopyPeek'`。
- **不要用 `ctest -j` 并行跑** —— 多进程用例共享 `/dev/shm/indurtdb_*` 命名空间，会互相干扰。
- `tests/smoke/` 免 GTest，靠返回码判定，因此**在没装 GTest 的机器上仍可冒烟验证**；`BUILD_TESTS=ON` 但找不到 GTest 时根 CMake 只跳过 GTest 部分，smoke 照常构建。
- 各测试用独立 `instance_id` 隔离（`test_mp_types` / `test_mp_peek` / `smoke_lifecycle` / `iso_A` …），新增测试请沿用此模式，不要复用已有名字。

## 仓库状态与已知不一致（开工前务必知道）

- 当前在 `docs/repo-cleanup` 分支，工作区有**未提交改动**：`CMakeLists.txt`、`src/utils/logging.cpp`，以及**未纳入版本控制**的 `tests/smoke/`、`verify.sh`。
- 仓库**一个 git tag 都没有**，且从未有过 `release/*` 版本分支 —— 新分支模型自2.2.0 起执行，之前的历史版本需手工补基线。
- **版本号有三个来源且已漂移**：`VERSION` 文件 = `2.1.0`，`CMakeLists.txt` 的 `project(VERSION)` = `2.1.0`，但 `include/indurtdb.hpp:12-15` 硬编码 `INDURTDB_VERSION_STRING "2.0.0"`。改版本要三处一起改。
- **仓库里一个 git tag 都没有**（`CLAUDE.md` 曾声称“已有 tag 2.1.0”，与实际不符）。
- `.gitignore` 里的全局通配很危险：`*.json` / `*.yaml` / `*.cmake` / `*.log` / `*.patch` / `*.lock` 全被忽略，**并且 `.github/workflows/` 也被忽略**（意味着 CI 配置无法提交）。新增这类文件时必须 `git add -f`。
- `.gitignore` 已忽略 `.omc/`、`.claude/`、`.cache/`、`docs/superpowers/`；`CLAUDE.md` 已明确允许根目录同时保留 `CLAUDE.md` 与 `AGENTS.md` 两个 AI 辅助文件。

## 协作与文档（详见 `CLAUDE.md`）

- **不自动 commit / push**：任何提交前先向用户确认；按逻辑单元合并提交，不搞“一改动一 commit”。
- **分支模型（release 分支制）**：`main` ← `release/<版本号>` ← `feature/xxx` / `fix/xxx`。
  每个版本一个版本分支；**feature/fix 一律合入所属版本分支，不得直接合入 `main`，也不得跨版本分支合入**。
  合入并验证通过后**本地 + 远程都要删掉 feature/fix 分支**；**版本分支本地与远程都长期保留**。
  版本分支开发完成 → 在其最新提交上**打 tag**，同步 `VERSION` + `CHANGELOG.md`。
- 不在 `main` 上直接开发。
- 流程走 SDD + TDD：需求 → 设计 → 任务 → 实施 → 交付；**方案获批前不动代码**。改动与现有架构冲突大时先提重构方案讨论，不要硬改。
- **文档是产品唯一对外接口**，代码与 `docs/` 必须同步更新。`docs/` 下目录名与文件名一律用中文并带数字前缀（`01-白皮书` `02-需求分析` `03-设计` `04-使用手册` `05-部署文档`…）。
- 这是 SDK 库：**公开头文件/接口变更必须保证二进制与源码前向兼容**，新增能力用追加新接口的方式，不要改既有签名、结构体布局或枚举取值。
- 评审要覆盖长期运行稳定性：内存泄漏、fd 泄漏、共享内存段残留、订阅表/点位表资源耗尽。工具：gtest（`-race` 对应本工程用 TSAN/ASAN 构建）、valgrind、ctest。Linux 用 gdb 调试。

## 文档索引（按需读，不要通读）

| 文档 | 路径 |
|---|---|
| 协作规范 / 工程流程 | `CLAUDE.md` |
| 编码规范（部分未落地，见上文警告） | `docs/03-设计/编码规范.md` |
| Seqlock 算法原理 | `docs/03-设计/Seqlock算法设计文档.md` |
| 概要 / 详细设计 | `docs/03-设计/InduRTDB 概要设计文档.md`、`docs/03-设计/InduRTDB 详细设计文档（LLD）.md` |
| SRS | `docs/02-需求分析/InduRTDB 需求规格说明书（SRS）.md` |
| API 参考 | `docs/04-使用手册/` |
