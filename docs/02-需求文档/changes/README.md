# 变更卡（Change Card）

> 工程规则 R7：**代码之前先有卡**。卡不是文档装饰，是评审与发布门的证据入口。

## 什么时候写

- 需求开发、缺陷修复、行为变更：**必须先写卡，再动源码/测试**。
- 纯文档、纯注释、纯工程管理（非行为变更）：免卡（如改动涉及发布，仍走发布门）。

## 命名与存放

- 文件名：`<id>.md`，`id` 建议 `<版本>-<主题>`，例如 `v3.11-T1-订阅规模化.md`。
- 模板：`模板-变更卡.md`（复制后填写，模板本身不参与检查）。
- 冻结基线：`<id>-baseline.json` **只本地留存、不入库**（`.gitignore` 已忽略 `*.json`）。

## 槽位与检查

必填槽位：`问题与影响` / `设计决策` / `验收判据` / `测试证据` / `文档影响` / `评审记录` / `发布/部署影响`。

```bash
python3 scripts/check_change_card.py docs/02-需求文档/changes/<id>.md
```

检查器会拒绝：占位符（待评审/TODO/TBD）、选项列表（`通过 | 有条件通过`）、空值（`无`/`-`）、
作者自审（`author`/`self`）、结论不在 `- 结论:` 行。占位即未完成。

## 冻结顺序（防「先写代码再补卡」）

```bash
# 首次源码/测试编辑之前
python3 scripts/freeze_baseline.py \
  --baseline docs/02-需求文档/changes/<id>-baseline.json \
  docs/02-需求文档/changes/<id>.md

# 评审时校验「卡早于第一个实现文件」
python3 scripts/check_baseline_order.py \
  --baseline docs/02-需求文档/changes/<id>-baseline.json --source src tests
```

基线只能证明「冻结之后发生了实现」，不能检测「冻结之前已经写了代码」，因此必须**立即冻结**。
