# SylixOS G2 · rtdbd 主路径移植方案分析 · 独立评审证据

> 评审对象：`docs/03-设计文档/22-2026-11-19-SylixOS-G2-rtdbd主路径移植方案分析.md`（v1→v2）
> 依据：21 号方案 §3.2/§3.3、q-B 裁决（本评审证据:50 / 21号:199）、G0/G1 与 atomic64 修复实施报告、`CODEBUDDY.md`《SDD/不外推纪律》。
> 两轮 fresh-context 独立评审：首轮（1 P0 + 2 P1 + 7 P2）→ v2 返工 → 终验通过。评审人未参与文档编写，全程只读。
- date: 2026-11-19

## 验证了什么

- reviewer_type: ai-independent
- reviewer_context: independent
- 逐条打开源码/SDK/文档核对（非照抄）：`rtdbd.c` 双通道（UDS `:2596-2790`、VSOA `:2308-2328/2630-2678`、poll 合并 `:2680-2735`、drain `:2854`）、写 dispatch 共用（`rtdbd_dispatch.c:98-139`，UDS `:1010`/VSOA `vsoa_adapter.c:486`）、`peer_cred` 唯一 getsockopt（`rtdbd.c:795-802`）、supervisor（`:1908-2050`、调用 `:2387-2389`）、订阅重建缺口（`defer_on_rebuild_subs rtdbd.c:2131-2140` 仅 install_subs(NULL,0)、断链 `vsoa_adapter.c:92-123` 未入队）、审计/health 缺口（`rtdbd_dispatch.c:297-314` 只 PING）、CMake 接入（`CMakeLists.txt:65-93/109-119`、`rtdbd/CMakeLists.txt:38-62`、`check_vsoa_sdk.py:18,55,76-82`）。
- 交叉 nm 实测 SDK 符号（`/opt/acoinfo/.../aarch64-sylixos-elf-nm -D`）：`vsoa_framework/lib/sylixos/aarch64` 下 `libvsoa-server.so` 11 符号（`create e290 / on_cli ec00 / add_listener ef14 / close e914 / start e480 / cli_address f410 / cli_datagram 101e4 / cli_reply f500 / cli_set_authed fcb4 / fds 10210 / input_fds 10360`）+ `libvsoa-client.so` 3 符号（`connect e284 / call f440 / input_fds e6a0`）全导出；四库大小 `333776/329408/9072/152896`；版本 `vsoa_version.h:17`=`1.9.1`。与文档 §2.4 逐字一致。
- 确认 21 号 §5:171 G2 原文退出判据含「订阅重建端到端通过」，本文建议收窄为「写入+NOTIFY」——差异已标注待 h-A 裁决、裁决后回填。
- 确认 q-B 契约层已替代 SO_PEERCRED（hello + rtdbd_authz 纯函数，VSOA 路径无 getsockopt）；client/监控台 UDS 依赖与 VSOA↔UDS 网关建议准确；RTC=2000 不污染排序的推理链（写 ts 前端传、NOTIFY ts 取单调钟、审计 ts=0）成立。
- 边界合规：无越界（未宣告 G2 完成/未填 B 段/未改对外口径/未授权开工/未碰设备）；发布门 FAIL 8、F2/F3a/F4 未替代；h-A~h-D 均为决策点而非擅断。

## 风险点

- P0（首轮，已闭环）：v1 把「SDK 4 符号导出」外推为「SDK 侧无移植障碍」，违反 21 号「不外推」纪律——G0/G1 只跑核心库 shm/原子/pthread，从未在 SylixOS 上执行 `vsoa_server_start` loopback bind 与 `cli_datagram` 收发。v2 已降级为「符号/ABI 已机械确认，socket 运行列 G2.3 硬门」，全文 6 处一致、无正向外推。
- P1-1（已闭环）：v1 符号硬门只覆盖 4 符号，不足以覆盖 adapter/harness 实际调用；v2 扩展为 11 server + 3 client 全集并实测。
- P1-2（已闭环）：v1 未标注本文 G2 范围与 21 号 §5:171（含订阅重建）冲突；v2 已在 §1.2/§7 标注待 h-A 裁决、裁决后回填 21 号。
- P2（7 项已闭环）：SO_PEERCRED 唯一调用点措辞（accept `:2770`）、Linux 专有头隔离面（sys/un.h/getopt.h/poll.h/unistd.h + 复用现有宏）、RTC 不污染排序推理链与 getopt 风险、G2.3 定向投递量化（`peer_notify_leak=0`）、§7 判据编号顺延、SDK 根路径来源（vsoa_framework 1.9.1）、共享依赖含 g_stats。
- 残余风险（非本文可闭环）：VSOA socket 在 SylixOS 的 bind/收发运行未验（G2.3 硬门）；订阅重建/审计/health 未落地（h-A/h-B 裁决另立）；SDK 升级符号回归（CMake FATAL 门固化）；getopt_long 在 SylixOS 可用性（G2.1 实测）。

## 结论与整改

- 结论: **通过**（两轮 fresh-context 独立评审：首轮 1 P0 + 2 P1 + 7 P2 → v2 返工 → 终验通过；P0 外推已清除、符号证据实测属实、边界合规、h-A~h-D 保留为决策点，可进入用户裁决）。
- 本文为 SDD 第一步（方案分析），不构成开工授权；实现须按裁决范围另立变更卡 + 冻结基线 + 独立评审 + 开工授权。
- 用户 h-A~h-D 裁决后：订阅重建/审计-health/网关按裁决另立变更卡；G2 范围若收窄须回填 21 号 §5:171；VSOA socket 运行硬门在 G2.3 实测，不外推符号可用为运行可用。

---

## 用户裁决落痕（2026-11-19）

用户以「G2 方案四项裁决单」对 h-A~h-D 作出书面裁决（均按推荐值），留痕如下：

| # | 裁决 | 结论与执行边界 |
|---|---|---|
| h-A 订阅重建 | **否，另立变更卡** | G2 收窄为写入+定向 NOTIFY 最短闭环；21 号 §5:171 已回填收窄后判据；订阅真实重建另立 |
| h-B 审计/health | **否，已知留白** | 最短路径不依赖；另立变更卡 |
| h-C VSOA↔UDS 网关 | **否，另立** | G2 不碰 client/monitor；网关另立；不新增 opcode、不改 ABI |
| h-D 落库/开工 | **聚合提交、暂不实现** | 22 号方案+评审证据+索引 1 commit（不推送）；实现按裁决范围另立变更卡+冻结基线+独立评审+开工授权 |

**边界**：本轮裁决确定 G2 范围；本文与 22 号方案均不构成开工授权；实际实现待相应变更卡与独立评审完成后执行；发布门仍 FAIL 8、对外平台口径不变、F2/F3a/F4 不因 G2 就绪。

**证据原始性说明**：本证据「结论与整改」末条原写「用户 h-A~h-D 裁决后…若收窄须回填」，系评审当时状态；为保留原始记录，该正文不改写，当前有效状态以本节裁决落痕与 22 号 §6.1 为准（均为已裁决、已回填）。
