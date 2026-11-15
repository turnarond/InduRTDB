# M5 阶段（B 段真实栈取证与闭环）· 方案分析/执行计划 · 三阶段独立评审证据

> 评审对象：`docs/03-设计文档/19-2026-11-14-M5阶段B段闭环方案分析.md`（v1→v2）、`docs/03-设计文档/20-2026-11-14-M5阶段B段闭环执行计划.md`（v1→v2）
> 依据：13 号裁决 §5/§9.4/§10/§11、14 号实现期计划 M0–M7、16/17/18 号 M3.3 切片规划与留白、`CODEBUDDY.md` 工程闸门/SDD/工具纪律；用户 2026-11-14 裁决「M5 方案分析与计划」。
> 三轮评审均 **independent fresh context，只读**，被评审文档作者未自审；两路首轮评审并行独立、互不知晓对方结论。
- date: 2026-11-14

---

## 第一签：规格合规（code-explorer，fresh context，只读）

- reviewer_type: ai-independent
- reviewer_context: independent
- 结论: ❌ **Issues found**（3 Major + 5 Minor；契约名/字段/阈值/红线大体忠实，引用真实率高）

**Major**：
1. `20` 号 P0–P7 与 14 号 M0–M7 退出门错配（P0 标 M0 但 M0 已执行；P1 退出门用 `14:320` 风险缓解代替 `14:308`；P3 漏 `14:307` M1 门禁三项）。
2. 把已入库永久脚手架 `vsoa_cluster_harness` 误判为「临时 POC、不入库」（与 `14:170`、`vsoa_cluster_harness.h:13-14` 冲突；`13:365` 只约束 `/tmp` POC）。
3. `reviewer_type=ai-independent` 判据张冠李戴并在冻结契约外新增机械判据（实测 `check_release_conditions.py:75-79` 只判文件存在；`ai-independent` 强制在 `release_gate.py:99-105` 且只作用于 HC1–HC4）。

**Minor**：P5 `verified_by` 留空与 `--json PASS` 机械冲突；tdd 档位头部未限定范围；13 号 §11.1 冻结的 7 个 fixture 路径未逐字转述；`rtdbd_defer_actions.h:28` 轻微错行；风险表把 q2 条件结论压缩成无条件断言。

**通过项**：阈值 T1–T5 与 `13:77-81` 逐条一致、无放宽；ctest 名与 `fields` 字段逐字一致、无改名；`18:45` / `台账:57` 引用属实；纪律红线无隐含开工/手改快照/预填数字/自置 `verified_by`；q2 的 ON-only vs OFF 快照事实基础成立。

---

## 第二签：方案质量（code-reviewer，fresh context，只读）

- reviewer_type: ai-independent
- reviewer_context: independent
- 结论: **Changes requested**（3 Critical + 6 Important + 6 Minor；6 条必须修复项）

**Critical**：
1. **C1**：v1 采信 M3 期旧证据 `v3.12-M3-IR证据.md:849-859`，把 3 项已落地能力误判为未实现，且污染 `20` 号 P3 任务清单（`sync_peer` 来源校验 `rtdbd_authz.c:66-76` + `vsoa_adapter.c:172`；`on_reconnect` `rtdbd.c:2192`；变更通知定向投递 `vsoa_adapter.c:494-496`，`:472` 为过时注释）。方案 A 否决与推荐 B 仍成立，但须按源码重写。
2. **C2**：P5 退出门机器可复现硬死锁 —— `verified_by` 留空 vs `check_release_conditions.py --json PASS`（`:75-79` 强制非空且 `is_file()`），P5 永不退出、P6/P7 不可达；矛盾继承自 `14:286-287` 而 v1 未识别。
3. **C3**：防自审核心机械校验被写成存在，实际不覆盖 B 段 7 项（任意 `.md` 自填 `verified_by` 即可通过）。

**Important**：I1 q2 把方案该自答的快照口径推给用户、删「本方案新发现」措辞；I2 P1 `IR7` RED 对 P2 注册的循环依赖 + harness 定性 + F3/F4 覆盖；I3 测点缺时钟源/可见性/样本百分位/RSS 计量点/T1–T5 落库容器；I4 ③④ 解锁与写死 `n/a` 冲突、`gate_label` 与冻结 ctest 名同名、P6 缺「数字是抄的还是测的」挑战项；I5 q4 缺方案判断、q3/q5 伪裁决；I6 两文档验收判据不一致且「验 FAIL 8 需调发布门」与红线互斥。

**Strengths**：口径差异诚实声明、门禁静态匹配判断准确、主动订正 18 号旧状态、禁语与防假绿基调正确、无编造实测数字、工期克制、红线与定位清晰、§5.3 契约字段对齐。

---

## 第三签：v2 修订闭环独立复验（code-explorer，fresh context，只读）

- reviewer_type: ai-independent
- reviewer_context: independent
- 结论: **有条件通过**（11 条修订 10 条完全闭环、1 条实质达标仅清单自述措辞不符；3 条 Critical 全部经源码/契约原文独立验证闭环；无 Blocker）

**逐条**：C1 伪缺口订正、C2 分阶段退出门、C3 防自审归属、I1 q2 拆分、I2 循环依赖/harness/F3/F4、I6 验收判据统一、I3 测点可复现、I4 解锁语义、I5 q4 判断、Major-1 里程碑映射均判「闭环」；Minor 子项 7/8 闭环，1 项为 §9 自述与文档实际不符。

**签字前必须修复**（均为文档文字）：① `rtdbd.c:486` → `vsoa_adapter.c:486`（两文档重复，实测 `rtdbd.c:486` 为空行）；② `check(require_second=True)` 与 `ai-independent` 强制的能力归属精确化（前者允许 `human`，强制逻辑在 `release_gate.py:102-105`）；③ §9 第 11 行改「订正为字段语义注释（非当前状态）」。
**建议项**：F3 拆 F3a/F3b 消残留循环；裸锚点 `:28-31` 补文件名。

---

## 验证了什么

- **controller 独立核实源码事实（不采信任一报告）**：`rtdbd_authz.c:66-76` 已实现来源校验、`:28-31` 为「不可达占位」注释；`rtdbd.c:2192` 已接 `exec.on_reconnect`、`:2189-2191` 记 VSOA 断链入队点未接入；`vsoa_adapter.c:494-496` 已 `vsoa_notify_dispatch()`、`:472` 为与同函数矛盾的过时注释；`vsoa_cluster_harness.h:13-14` 明写「永久测试脚手架，故入库」；`check_release_conditions.py:75-79` 强制 `verified_by` 非空；`14:170` 记 M2 永久脚手架入库；`rtdbd.c:486` 为空行、VSOA 写路径直调在 `vsoa_adapter.c:486`、UDS 侧见 `rtdbd.c:1010`。
- **v2 修订闭环**：19 号 §9 列 11 条修订位置并追加第 12 条「第三签字前订正」留痕；3 条必改 + 2 条建议全部就地订正（`vsoa_adapter.c:486` + `rtdbd.c:1010`、`ai-independent` 归属、§9 措辞、F3a/F3b 拆分、`rtdbd_authz.c:28-31` 补名）。
- **机械校验（controller 实跑）**：`bash scripts/check_all.sh`（full，禁 `--quick`）→ `CHECKALL_RC=0`、`100% tests passed, 0 tests failed out of 53`、`OK: 9 个守卫均可证伪`、`ALL GREEN`（`build/check-all-20261114-165341.log`）。
- **发布门零回归**：`check_release_conditions.py --ledger` → `FAIL: 7 项未完成（B 段数字落库或证据缺失）`；台账 `required_irs` 与 `build/ctest-v3.12-snapshot.txt` 零 diff ⇒ FAIL 8 的第 8 条结构性不变（**本轮未调用 `release_gate.py` 放行门**，符合 19 号 §8 第 3 条订正后的判据）。
- **纪律零 diff**：`git diff --stat` 对 `VERSION`/`CMakeLists.txt`/`include/indurtdb/indurtdb.h`/`README.md`/`v3.12-B段数字落库.json`/`v3.12-条件闭环台账.json`/`build/ctest-v3.12-snapshot.txt` **全为 0 行**；工作区仅 2 份新文档 + 3 个 `examples/*` 未跟踪二进制（非本任务产物）。
- **交付内容核对**：19/20 号 v2 均为纯 Markdown；未改生产代码/`tests`/`scripts`、未注册 ctest 名、未填 B 段 `fields`、未刷快照、未调放行门、未打 tag、未改对外口径、未 commit/push；`docs/README.md` 索引由 P0.4 同步登记 15–20 号。
- **评审证据自检**：本文件含 `date`/`reviewer_type`/`reviewer_context`/`验证了什么`/`风险点`/`结论与整改`，`check_review_evidence.py --require-second` 通过。

## 风险点

- **现场条件不可得**：6 节点拓扑（F2）、网络注入（F3a）、设备整机重启（F4）本机不具备，B 段 P3/P10/P11 与 ③④/T1/T2 真实判据无法在本机取证；`20` 号 §1.1 已定 P5–P7 BLOCKED 终止分支，不得填数字或放宽 `EXPECTED_IDS`。
- **v2 仍为规划文档**：§5.5 双构建快照现场比对、§5.6 B 段 7 项机械防自审校验器、P4 `gate_label` 改名均为**未来任务（当前未实现）**，不得被读作既有能力。
- **契约变更待裁决**：q1（里程碑口径）、q2（`required_irs` 是否扩到 `Gate_T*`）、q4（开工口径确认）尚未经用户书面裁决；`20` 号 P1 及以后维持 BLOCKED，本轮不授权开工。
- **静态快照与文本匹配**：`release_gate.py:152` 只做 `name not in text`，`ir_snapshot` 为 OFF 的 52 项快照且不含 ON-only `IR5_NotifyDirection`；若 P5 仍用 OFF 快照，7 个 IR 全实现发布门仍 FAIL，须按 19 号 §5.5 方案自答口径落地并配坏样本。
- **旧证据时效**：`v3.12-M3-IR证据.md:849-859` 与 `B段数字落库.json:5` 均为 M3/2026-11-11 期状态，v1 曾据此误判；执行期须以源码与实时 `ctest -N` 为准，19 号 §2.2 已作时效订正。
- **ON 60/60 证据（原风险点第 6 条，2026-11-15 已解除）**：已重跑并归档 `docs/08-验证记录/artifacts/v3.12-m33-on-ctest-20261115.txt` + 可复现记录 `v3.12-M3.3合入全量门禁证据.md`；原始 `/tmp` 日志不再作为证据来源，详见文末「后续补证」。
- **发布门维持 FAIL 8 属设计意图**：HC1–HC4 虽 `closed=true`，B 段 7 项 `pending` + IR 未注册仍机械拦截；「闭环 ≠ 放行」（`13:380`），不得把本方案评审通过误读为可打 tag/发布。

## 结论与整改

- 结论: 通过（三阶段独立评审：第一签规格合规 Issues found（3 Major + 5 Minor）、第二签方案质量 Changes requested（3 Critical + 6 Important）、第三签 v2 修订闭环复验「有条件通过」；6 条必须修复项 + 第三签 3 条签字前必改 + 2 条建议项已全部由 controller 就地闭环，并经源码独立核实与机械校验。方案 A 否决 / 推荐分阶段 B / 纯文档不产数字的总体结论成立，无编造实测数字、无 N/A→PASS、无未来任务写成既成事实、无隐含开工授权；纪律红线完整、门禁全绿、发布门零回归。剩余事项为 q1/q2/q4 用户裁决与现场环境，属后续 P0/P1 阶段，不阻断本方案评审闭环）

---

## 后续补证（controller，2026-11-15）

- **P0.5 闭环**：`20` 号 P0.5 列出的「ON 60/60 证据归档」已完成。2026-11-15 重跑 `bash scripts/check_all.sh`（full，禁 `--quick`）→ `OFF_RC=0`、ALL GREEN 53/53、9 守卫（`build/check-all-20261115-110817.log`）；`cmake --build build-vsoa -j$(nproc)` → 零警告；`ctest --test-dir build-vsoa --output-on-failure` → `60/60`、50.83 s。
- **归档物**：`docs/08-验证记录/artifacts/v3.12-m33-on-ctest-20261115.txt`（原始日志 6533 B）、`docs/08-验证记录/v3.12-M3.3合入全量门禁证据.md`（命令/结果/口径与留白/复现方式）、`docs/08-验证记录/README.md` 现有记录表登记；19 号 §2.1、§8 第 1 条与 20 号 P0.5 同步订正。
- **原「风险点」第 6 条（ON 证据未归档）据此解除**；其余风险点、三签结论与「通过」判定**不变**。本补证仅更新事实状态，未修改任何评审结论。
- **纪律复查**：归档物为纯文本证据；未改生产代码/`tests`/`scripts`、未注册 ctest 名、未填 B 段 `fields`、未刷静态快照、未调放行门、未打 tag、未提交；红线零 diff 仍为 0，发布门仍 FAIL 8。

---

## 第四签：订正闭环复核签字（code-explorer，fresh context，只读）

- reviewer_type: ai-independent
- reviewer_context: independent
- 复核日期: 2026-11-15
- 复核结论: **有条件通过 → 补齐后通过**

**逐条独立核验**：第三签 3 条必改 + 2 条建议全部真闭环（`vsoa_adapter.c:486` / `rtdbd.c:1010` 锚点；`check_review_evidence.py:53-56` 允许 `human` + `release_gate.py:102-105` 收窄 `ai-independent`；§9 第 11 行措辞；`F3a`/`F3b` 拆分与 P1 入口；`rtdbd_authz.c:28-31` 补名）。「后续补证」三归档物真实、ON 日志 1–60 全 Passed 且含 `100% tests passed, 0 tests failed out of 60`；三签结论原文与「通过」判定未被改写。

**签字前 1 条文字订正**：20 号 §7 第 3 条未同步 19 号 §8 第 1 条的 P0.5 归档物清单 → **已补**；建议项「F3 泛指残留」已修 P1.2 自指，其余泛指按第四签判定不影响入口，记录为后续观察。

**覆盖缺口补齐（controller 实跑；第四签无命令执行能力，其未能独立验证的 3 类命令在此补证）**：
- `git diff --stat -- VERSION CMakeLists.txt include/indurtdb/indurtdb.h README.md docs/08-验证记录/v3.12-B段数字落库.json docs/03-设计文档/reviews/v3.12-条件闭环台账.json build/ctest-v3.12-snapshot.txt` → **0 行**（红线零 diff）；
- `python3 scripts/check_release_conditions.py --ledger …` → `FAIL: 7 项未完成（B 段数字落库或证据缺失）`；第 8 条因台账 `required_irs` 与静态快照零 diff 而结构性不变，FAIL 8 零回归；
- `bash scripts/check_all.sh`（full，禁 `--quick`）→ `CHECKALL_RC=0`、ALL GREEN 53/53、9 守卫（`build/check-all-20261115-111328.log`）；`git status --short` = 2 处 README 修改 + 19/20 号 + 本评审证据 + 2 个 P0.5 证据文件 + 3 个 `examples/*` 未跟踪二进制（非本任务产物）。

按第四签明确条件「补这一行文字并附三条只读命令输出后即可签『通过』」，条件已全部满足 → **四签闭环，结论：通过**。

---

## 用户裁决落痕（2026-11-15）

用户以「M5 方案收尾 · 一次性裁决单」对 q1/q2/q4 与提交作出书面裁决（均按方案推荐），留痕如下：

| # | 裁决 | 结论与执行边界 |
|---|---|---|
| q1 里程碑口径 | **按 14 号原定义** | 「M5 阶段」仅作措辞别名；P0–P7 映射与退出门**零改动**，判据逐项引用 14 号 M0–M7 |
| q2 发布门判据 | **扩到 `Gate_T*`** | 方向已定：T1/T2/T3/T4/T5 冻结用例名纳入机械检查；**属契约变更，实施留 P4/P5**（同步台账 `required_irs`、`release_gate.py` 判据、canary 坏样本），**本轮零改动、不改冻结用例名**（`14:263`） |
| q4 开工口径 | **确认未越界** | 允许在 `release/v3.12` 等版本分支写 B 段实现；合入 `main` / 打 tag / 发布**仍严禁直到 `release_gate.py` 放行**。P1 仍被 F2/F3a/F4 现场条件阻塞，**本确认不等于立即开工** |
| 提交 | **聚合提交** | 7 个纯文档/证据文件合成 1 个 commit，排除 `examples/*` 3 个二进制；不改生产代码 |

**边界**：本轮裁决只定方向与授权，**不实施任何契约变更、不写实现代码、不改台账/校验器/canary/快照/版本号/对外口径**；19 号 §6、20 号 §0/§1/§6 同步落痕。三签/四签原文与「通过」结论不变。

**证据原始性说明**：原「风险点」第 3 条（契约变更待裁决）与「结论与整改」中「剩余事项为 q1/q2/q4 用户裁决」系评审当时状态，已于 2026-11-15 因用户书面裁决而解除/更新；为保留评审原始记录，上述正文**不改写**，以本节裁决落痕为当前有效状态。
