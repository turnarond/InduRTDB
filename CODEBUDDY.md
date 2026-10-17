# CODEBUDDY.md This file provides guidance to CodeBuddy when working with code in this repository.

## 项目定位

InduRTDB 是**纯 C11 实现的工业实时数据库**，定位为边缘控制的**跨进程共享内存实时数据层**（数据链路中间的热数据层）。

| 做 | 不做 |
|---|---|
| 跨进程共享内存点位存储（低延迟、确定性）、工业语义（quality/timestamp/unit/access）、订阅、崩溃自愈 | 南向设备驱动（Modbus/OPC UA/S7）、北向 SCADA 对接、持久化/历史库、集群同步 |

它作为 [node-server](https://github.com/acoinfo/edge-framework)（BAS Edge Data Hub）的底层数据层被集成；北向与持久化能力复用 node-server，不重复实现。当前版本 **v3.7.0**（README / `VERSION` / CMake `project(VERSION)` / `indurtdb.h` 版本宏四处一致）。

## 常用命令

**配置与构建**（顶层，默认 Release）
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
```
只在 `CMAKE_SYSTEM_NAME` 为 Linux/SylixOS 时可构建；Linux 下自动选 `src/osal/posix`，SylixOS 下选 `src/osal/sylixos`，其他平台 `FATAL_ERROR`。产物为静态库 `build/libindurtdb.a`。

**运行全部测试**
```bash
cd build && ctest --output-on-failure -j$(nproc)
```
需先安装 GTest（`find_package(GTest REQUIRED)`）。**37 个 ctest 用例**（25 单元 + 12 集成），gtest 层面共 **266 个用例**。引用测试数量时须说明是哪一层——这两个数字不是同一口径，别混用。

**运行单个测试**
```bash
cd build && ctest -R test_c_shm --output-on-failure      # 按 ctest 名（=源文件名去扩展名）
./build/tests/unit/test_c_shm --gtest_filter=Seqlock.*   # 直接跑二进制并过滤用例
```
单元测试名即 `tests/unit/test_c_*.cpp` 的文件名；新增文件后需重新 cmake 才会被 `file(GLOB)` 收录。

**长稳与故障注入（不接入 ctest）**
```bash
bash scripts/run_soak.sh          # 默认 10s；SOAK_DURATION=30 或位置参数可改
```
脚本自行编译 `tests/soak/soak_test.c` 与 `fault_injection_test.c`，验证多进程持续读写数据完整性、owner 崩溃接管、奇数 write_seq 恢复，并清理 `/dev/shm` 残留。

**性能基准**
```bash
bash scripts/run_bench.sh --quick      # 快速模式；WARMUP/ITERS/BATCH_CNT/TP_SEC 可覆盖
```
输出 P50/P99/吞吐。设计目标 **P99 ≤ 10μs**，改动读/写热路径后必须跑，回归数据需回填 README 表格。

**UDS 延迟基准（v3.3 架构验证）**
```bash
bash scripts/run_uds_bench.sh                 # 默认 16B 报文
MSG_SIZE=48 bash scripts/run_uds_bench.sh     # 接近真实写请求的尺寸
```
测量 Unix Domain Socket 往返延迟（P50/P99/P99.9/吞吐），用于验证读写分离架构的延迟预算。实测参考（x86）：16B 时 P50 ≈ 11.6μs / P99 ≈ 18.2μs；48B 时 P50 ≈ 12.8μs / P99 ≈ 24.0μs。详见 `docs/06-开发规划/11-v3.3-T0-UDS延迟实测报告.md`。

**外部消费回归**
```bash
bash scripts/verify_consume.sh
```
在临时目录完成 install，验证 find_package、pkg-config（含 prefix 一致性）、FetchContent 三条消费路径 + `templates/indurtdb-starter`。改安装规则/CMake 导出后必跑。

**零警告检查（CI 等价）**
```bash
cmake --build build 2>&1 | grep -E 'error|warning'
```
库编译开启 `-Wall -Wextra -Werror`，任何警告即失败。CI 为 Debug/Release 双配置矩阵（`.github/workflows/ci.yml`）。

**示例与 demo**
```bash
cd examples && make && ./basic_example                      # 直连 ../build/libindurtdb.a
cd demo && make INDU_PREFIX=<prefix> && make run            # 经 pkg-config 消费已安装库
```

## 架构总览

### 分层与依赖方向

单向依赖，**不允许反向引用**：

```
include/indurtdb/indurtdb.h   唯一公共头文件
                              v1 单例 API 62 个（已标 INDURTDB_DEPRECATED，B5）
                              v2 句柄 API 50 个（indurtdb_h_*，v3.4 T4 去单例化）
        ↓
src/api/indurtdb.c            v1 外观层：全局单例 g_rtdb，串联各模块、错误码、fork 检测
rtdbd/                        v3.3 写权威服务进程：UDS 串行写 + UID 鉴权 + 审计 + 背压 + RBE 通知门控
client/                       客户端库：RPC 读写订阅，服务不可用时本地排队重放
        ↓
src/core/                     irt_shm / irt_point_manager / irt_subscription / irt_config
                              irt_semantics（死区/EURange 纯函数） irt_index（name→id） irt_delta（运行时注册表持久化）
        ↓
src/internal/                 irt_types.h（共享内存布局） irt_seqlock.h（无锁原语）
src/osal/                     irt_osal.h + posix/ + sylixos/（构建期选择实现，无虚表）
```

> v1 单例 API 已进入退役轨道（v3.7 B5 标记 deprecated，**删除推迟到 v4.0**）。新代码一律用 `indurtdb_h_*` 句柄 API；改动 v1 时只做必要的兼容修复，不要新增功能。

`g_rtdb` 是一个文件域静态结构体，持有 `irt_shm_t / irt_pm_t / irt_sub_t / initialized / owner_pid`。核心层**无堆分配**（规范禁止 malloc/free），全部为定长数组与栈缓冲，因此 7x24 运行无内存碎片。错误信息用 `_Thread_local char g_last_error[256]`，无需锁。

### 共享内存布局 —— ABI 硬约束

实例段名 `/indurtdb_<instance_id>`，一段连续内存。**v3.4 起为「布局 v2」**——不再是「Header + 点位 + 心跳」的线性排列，而是四个区段，偏移写进 Header 的 `off_*` 字段，访问点一律读 `off_*` 而非按 `sizeof(header)` 硬算：

```
irt_header_t (128B, aligned 128)
  ├─ 点位区   indurtdb_point_t × max_points (128B)
  ├─ 索引区   name→id 开放寻址索引   (off_index)
  ├─ 元数据区 每点 32B 冷数据（EURange/deadband/flags）(off_meta)
  └─ 订阅区   irt_subscriber_entry_t × max_subscribers (16B) (off_subs)
```

`irt_header_t` 含 `magic(0x1DBA1DBA)`、`version`、`max_points`、`max_subscribers`、`write_seq`（全局 Seqlock 序列号）、`owner_pid`、`stats{writes,timeouts}`，以及布局 v2 新增的 `crc32`（Header 完整性校验）、`flags`、`off_points/off_index/off_meta/off_subs` 四个区段偏移、`index_count`、`scan_skipped`。点位为 POD，**禁止含指针**。多组 `IRT_STATIC_ASSERT` 在编译期锁死各字段偏移与结构体大小。

> ⚠️ **Header 从 64B 扩到 128B 属于 ABI 变更**，段版本从 1 升到 2，**v1 段拒绝挂载**。这意味着 v3.4+ 无法直接复用 v3.3 及更早创建的段——升级需停机，配套 `irt_migrate` 工具。任何改动 `irt_types.h` / `indurtdb.h` 结构体的操作都必须先评估 ABI 影响。

### 并发模型

**全局单个 Seqlock**（`header->write_seq`），读路径完全无锁、无内核锁、无 futex：

- 写端：`irt_seqlock_write_begin()` CAS 循环把偶数 seq 变奇数（奇数表示写中，冲突返回），`irt_seqlock_write_end()` 写回 `seq0+2`。
- 读端：**没有** `irt_seqlock_read()`（因返回裸指针会在校验窗外读数据、存在 TOCTOU 脏读，已刻意移除）。`irt_seqlock.h` 注释给出范式，各读路径自行实现重试循环：`s0 = load(ACQUIRE)` → 若奇数 continue → 拷贝数据 → `atomic_thread_fence(ACQUIRE)` → `s1 = load` → `s0 != s1` 则重试。
- Seqlock 临界区内**禁止调用外部回调**。

`indurtdb_peek()` 走单拷贝，返回 `_Thread_local` 缓冲指针，**同线程下次 peek 即被覆盖**（需长期持有请用 `indurtdb_read_point()`）。写后通知订阅者时用 `irt_pm_read()` 读栈变量而非 peek，因为回调可能重入写路径覆盖线程本地缓冲。

### 多进程生命周期与崩溃自愈

- owner 判定：`shm_open(O_EXCL)` 原子竞争，owner 负责初始化 header。
- 崩溃恢复：attacher 通过 `kill(pid, 0)` 存活检查发现 owner 已死，调用 `irt_shm_os_claim_ownership()` 接管所有权。
- Seqlock 奇数恢复：attach 时若发现遗留奇数 `write_seq`（写锁内崩溃）自动推进到偶数，恢复一致性。
- fork 安全：`indurtdb_initialize()` 比较 `g_rtdb.owner_pid != getpid()` 判定子进程，先清除 `os.owner` 再 shutdown，避免子进程 `shm_unlink` 误删父进程段。这是已修复的真实缺陷，改动该段逻辑前先看注释。

### 订阅与配置

订阅分两层，取决于走哪条路径：

- **v1 进程内回调**：`irt_sub_t` 持 256 个定长槽位（`IRT_SUB_MAX_CALLBACKS`），回调指针不进共享内存；共享内存只存心跳表（pid + `last_heartbeat_ns`），`irt_sub_cleanup_zombies()` 清理超时条目。**此路径回调不跨进程。**
- **v3.3 起跨进程通知**（推荐路径）：写操作收敛到 `rtdbd` 进程，由它经 UDS 向订阅客户端推送 `NOTIFY`。v3.7 主题 A 进一步加了 **按订阅者的 RBE 门控**——仅在死区越过或 EURange 边界跨越时推送，首值必发。

**绕过 `rtdbd` 直写共享内存的点位不会产生跨进程通知**，这是架构上的既定取舍（多写者会破坏写权威与通知一致性）。

配置：`irt_config.c` 自带轻量 key=value 解析与 YAML 点位元数据解析，不引入第三方库。注意 `.gitignore` 忽略 `*.yaml/*.yml`，新增配置样例需 `-f` 强制入库或改用 `.txt` 后缀。

### 构建产物与消费方式

静态库 + ALIAS `InduRTDB::indurtdb`。`INDURTDB_IS_TOP_LEVEL` 做子项目隔离：作为子项目（FetchContent/add_subdirectory）时默认关闭 tests/examples 且**不安装**。顶层安装导出 `InduRTDBConfig/Version/Targets` 与 `indurtdb.pc`，`indurtdb.pc` 的 prefix 在 configure 阶段写入——安装前缀必须用 `-DCMAKE_INSTALL_PREFIX` 指定。

### 测试体系

- `tests/unit/`：C++17 + GTest 包装纯 C API（库本体零 C++），每个 `test_c_*.cpp` 一个 ctest 用例。
- `tests/integration/`：12 个用例——`test_c_multi_process.cpp`（fork 多进程 + **原始字节布局回归**，ABI 防线关键）、`test_e2e_mixed.cpp`，以及 `test_rtdbd_*` 系列（core / proto_v2 / notify / health / config / backpressure / recovery / delta）与 `test_irtcli_queue.cpp`。
- `tests/soak/`、`tests/bench/`：纯 C，不经 ctest，由 `scripts/` 下脚本编译运行。
- `test_c_version` 校验版本宏与 `VERSION` 一致；`test_c_layout_seqlock` 校验布局与并发语义；`test_c_layout_v2` 校验布局 v2 的区段偏移与 CRC。

## 关键不变式（改代码前必读）

1. **零动态内存分配**：库代码禁止 malloc/free；缓冲区编译期定长。
2. **零 C++ 依赖**：库本体不含 STL/异常/RTTI/虚表；C++ 仅出现在测试。
3. **只用 `__atomic_*` builtins**，禁用 `stdatomic.h`（SylixOS 支持不完整）。
4. **布局字节兼容**：改 `irt_types.h` / `indurtdb.h` 结构体前，先确认与 v3.4 布局 v2 的兼容性与静态断言；改 Header 大小即段版本变更，旧段拒绝挂载。
5. **版本号四处同步**：`VERSION`、CMake `project(VERSION)`、`indurtdb.h` 宏、`README`/`CHANGELOG`。
6. **读路径无锁**：不要在读路径引入锁或系统调用。
7. **热路径零 meta 读**：默认 `indurtdb_write_*` 强制 `quality = GOOD` 且不读点位元数据。语义字段（EURange/死区）只允许经 rtdbd 的质量感知写入口进入，违反会让 P99 回归。
8. **RBE 状态不进共享内存**：按订阅者的上报状态存在 rtdbd 进程内存，避免 ABI 变更。
9. **写权威唯一**：跨进程通知依赖写收敛到 `rtdbd`。直写共享内存的点位不产生 `NOTIFY`，这是设计取舍不是缺陷。
10. **文档即对外接口**：改 API/架构/行为必须同步 `docs/` 对应文档，防止文档腐败。

## 仓库与协作约定

- **禁止直接 commit / push**：提交需用户确认；commit 要精简聚合，避免每个小改动一个提交。
- **分支模型（release 分支制）**：`main` ← `release/<版本号>` ← `feature/xxx` / `fix/xxx`。
  - 每个版本对应一个版本分支 `release/<版本号>`，本地与远程**都长期保留**。
  - 所有 `feature/` / `fix/` 分支**一律合入所属版本分支**；不得直接合入 `main`，也不得跨版本分支合入。
  - **保持分支整洁**：合入并验证通过后，`feature/` / `fix/` 分支**本地与远程都要删除**。
  - 版本分支开发完成时打 tag，并同步 `VERSION` / CMake `project(VERSION)` / `indurtdb.h` 版本宏 / README·CHANGELOG。
  - 版本分支收尾后不再接受新特性，只接受 bugfix；确需追加功能则另开新版本分支。
- **保持仓库干净**：不得把 `.superpower`、`.claude`、`.omc`、`.codegraph` 等 AI/插件中间目录，以及 `build/`、`build-*/` 构建产物提交入库。
- **中文优先**：沟通与文档一律中文（专有名词除外）。
- **文档规范**：`docs/` 按 `01-白皮书`、`02-需求文档`、`03-设计文档`、`04-技术文档`、`05-SDK手册`、`06-开发规划` 数字前缀排序，文件与目录名用中文；文档是唯一对外接口，须始终最新。
- **TDD 红-绿-重构**：每次实现必须附带测试代码或指明要变绿的用例；用例需经 `ctest` 注册以便 CI 复用。
- **工程化流程走 SDD**：需求 → 方案设计 → 任务规划 → 实施 → 交付，关键阶段（尤其方案设计）需专家评审。
  **判据：要不要动别人依赖的接口或内存布局。**
  - 要动（公开 API 签名 / 共享内存布局 / 段版本 / 协议 opcode / 错误码契约）→ 走完整 SDD，含方案评审
  - 不动（补 bug、加内部实现、补文档与脚本、加配置样例）→ 直接做，走 TDD 即可
  - 判据的由来：2026-10 一次「加写重试 API」的小任务中，实施阶段连续两次推翻了初始方案
    （错误码契约与文档不符；固定次数重试这一模式本身不成立）。跨 2 个文件时尚可收敛，
    同一误解若发生在跨十几文件的版本级特性上（例如点位分区要同时动索引 / 权限模型 /
    协议 / CLI / 监控台），范围会静默膨胀。方案评审正是为拦这个而存在。
- **SDK 向前兼容**：升级涉及头文件/接口变动时保证向前兼容，避免二进制 ABI 不匹配迫使使用者重编。
- **长稳视角**：评审与测试须考虑内存泄漏、句柄（fd/shm）泄漏等长期运行问题。
- **根因定位**：测试失败要追问到业务根因与技术约束根因（Linux 用 gdb，Windows 用 cdb 或转 WSL），禁止循环打补丁；修复后清理临时补丁以免误导。
- **不越界修他人问题**：排查可以，最终修复以 issue 形式提给对应工程/SDK/文档供应商。
- **重构需讨论**：当改动与整体架构冲突大、别扭或影响面广时，先讨论再重构。
- **工具集**：按需建设配套工具，优先做成 Python 系列工程（测试/部署/冒烟验证），工具自身要有工程架构。
