# SylixOS 适配可行性分析（SDD 第一步）· 独立方案评审证据

> 评审对象：`docs/03-设计文档/21-2026-11-19-SylixOS适配可行性分析.md`（v1 → v2 修订）
> 依据：用户 2026-11-19「做 SylixOS 适配可行性分析（新开一轮 SDD）」裁决；`CODEBUDDY.md`《SDD》《工程闸门与流程》《不越界修他人问题》；13/14/19/20 号既有真源。
> 评审人 fresh context、只读；被评审文档作者未自审；评审中实际打开源码/SDK/脚本逐条核对，未修改任何文件。
- date: 2026-11-19

---

## 验证了什么

- reviewer_type: ai-independent
- reviewer_context: independent
- 技术论断逐条独立核实（打开源码/SDK/脚本核对，非照抄文档）：
  - OSAL 仅 7 函数、仅时间+共享内存两类；SylixOS 实现是 POSIX 逐行拷贝（唯一差异 `CLOCK_MONOTONIC_RAW`→`CLOCK_MONOTONIC`，`src/osal/posix:22` vs `src/osal/sylixos:29`），无 SylixOS 原生 API、无空壳/TODO/ENOSYS。
  - CMake 平台分支属实：`rtdbd`/client/tests/examples 被 `if Linux` 排除；VSOA 探测块 `CMakeLists.txt:68-93` 被 `if(ENABLE_VSOA AND Linux)` 包住，显式 ON 也静默无效；全仓无 toolchain 文件。
  - 核心库未封装直接调用属实：`kill(pid,0)`（`irt_shm.c:141`）、`__atomic_*`（多文件）、`open/fopen`、`getpid`；`/dev/shm` 未硬编码（用 `IRT_SHM_PREFIX`，`irt_shm.h:16`）。
  - VSOA SDK 官方支持 SylixOS 属实：`lib/sylixos/` 实测 11 架构（含 32 位 arm/csky/mips/riscv/sparc 与大端 powerpc/sparc），`BUILD_AND_INSTALL.md:56-64,182-187`、`config.mk:23-25`、`vsoa_platform.h:59-73` 均有 SylixOS 支持；版本以宏 `VSOA_LIB_VERSION`=1.9.1 为准（`vsoa_version.h:17`，14 号:87 佐证），非 `VERSION` 文件的 1.7.0-rc1。
  - rtdbd Linux 依赖锚点准确：`SO_PEERCRED`（`rtdbd.c:794-800`）、`/run/indurtdb` 路径（`:2204`）、VSOA loopback（`vsoa_adapter.c:615`）、`cli_datagram`（`:261`）、supervisor `/proc/self/exe`（`rtdbd.c:1963,1992`）；未见 eventfd/epoll/inotify/prctl 等重大遗漏；「主写入口已切 VSOA loopback、Linux 依赖集中 legacy UDS/supervisor」的核心判断有源码支持。
  - 引用文档锚点准确：`13:202`（shm 掉电语义）、`19:167`（RAW/跨机时钟）、`01:179`（shmCreate 规划）、`08:320`（toolchain 素材）、`07-部署运维/README.md:71`（平台口径）。
  - 边界合规实测：`check_release_conditions.py --json` → 7 项全 pending、RC=1，与文档 §4「FAIL 8」一致；本文未宣告 F2/F3a/F4 就绪、未填 B 段数字、未改对外口径、未授权开工、未碰设备 192.168.30.177、未调用 release_gate.py。
  - 证据可证伪性：约 30 处锚点逐条核对均准确，无编造行号/架构数/函数名。

## 风险点

- P1-1（已闭环）：原 v1 遗漏「跨 32/64 位 ABI、大小端、SylixOS shm 容量上限」维度——SDK 11 架构含 32 位与大端，而 `src/internal/irt_types.h:59-105`、`protocol.h:160-417`、`client.h:100,128` 的编译期断言是移植的核心机械判据；不补则 §3.1「验证债非实现债」结论缺论证。v2 已新增 §3.6 并在 §0 表、§5 G0/G1、§6 风险表同步。
- P1-2（已闭环）：原 v1 漏算 `scripts/check_vsoa_sdk.py` 改造量——该脚本 `:18` 硬编码 `lib/linux/x86_64`、argparse 无平台参数，CMake `:76-83` 未透传 `VSOA_LIB_SUBDIR`，现状无法探测 `lib/sylixos/<arch>`，导致 G0「脚本可校验」判据不可达。v2 已在 §2.4 补改造清单、§3.2/§5 G0 同步。
- P2-1（已闭环）：工具链论断过度保守——`aarch64-sylixos-elf-gcc` 实测在 PATH（`/opt/acoinfo/realevo-stream/compiler/.../bin/`）。v2 §2.2 改为「已实测在 PATH，目标架构/板级可用性待 q-A 确认」。
- P2-2（已闭环）：`CLOCK_MONOTONIC_RAW` 缺失证据不足——无 SylixOS 用户态 `<time.h>` 证据。v2 §3.5 改为「OSAL 未用 RAW，BSP 待证实，疑似缺失」，与全文待实测纪律一致。
- P2-3（已闭环）：§0 SDK ✅ 可能断章取义。v2 在 §0 表与 §2.4 明确「仅指 libvsoa SDK，非 InduRTDB 产品支持」。
- P2-4（已闭环）：`SO_PEERCRED` 风险评级内部张力（标「最高」但 UDS 可裁）。v2 §2.5 改为「UDS 启用时最高，裁撤则降中」。
- P2-5（已闭环）：`docs/README.md` 缺 21 号登记。v2 已补登记并标注不改变平台口径、不授权开工。
- 遗留风险（非本文可闭环）：q-A 目标架构是否在预编译列表、q-B UDS 写裁撤的契约接受度、q-C 工具链/BSP 引入方式、q-D 是否仅到方案评审为止——四项开放问题待用户/后续评审裁决，不阻塞本文作为方案分析通过。

## 结论与整改

- 结论: **通过**（v1 无 P0 致命问题；2 项 P1 + 4 项 P2 均已闭环于 v2；关键论断约 30 处锚点逐条核实准确、边界纪律合规、证据可证伪）。
- 本文可进入下一阶段（作为 SDD 第一步成果）；G0 起的实现属行为/契约变更，须另立变更卡 + 冻结基线 + 独立评审 + 开工授权，本文不构成任何开工授权。
- q-A～q-D 四项开放问题由用户在后续阶段裁决；裁决前不改对外平台口径、不宣告 F2/F3a/F4 就绪、发布门维持 FAIL 8。
