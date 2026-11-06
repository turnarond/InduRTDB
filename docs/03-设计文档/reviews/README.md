# 评审证据（Review Evidence）

> 工程规则 R17：**作者自审不算第二签**。第二签 = 独立上下文的 AI 评审（`ai-independent`）或人类评审（`human`）。

## 命名与存放

- 文件名：`<id>.md`，与变更卡 `<id>` 一一对应（例如 `v3.11-T1-订阅规模化.md`）。
- 模板：`模板-评审证据.md`。

## 实质性评审三要素（缺一即无效）

1. **验证了什么**：实际跑过的命令/测试、读过的关键代码路径，不是「看了一遍」。
2. **风险点**：至少 1 条具体风险或改进项；写「无」必须说明为什么无。
3. **结论与整改**：`通过` / `有条件通过（列条件）` / `返工（列原因）`，并给出复验方式。

## 检查

```bash
python3 scripts/check_review_evidence.py docs/03-设计文档/reviews/<id>.md --require-second
```

检查器会拒绝：占位符、选项列表未删、`reviewer_type` 非 `ai-independent`/`human`、
`reviewer_context` 非 `independent`、无具体风险条目。

## 回填闭环

评审完成后必须回填变更卡 `## 评审记录` 并重跑 `check_change_card.py`；
卡与证据的结论不一致（一方通过、一方返工）时发布门 `release_gate.py` 直接 STOP。
