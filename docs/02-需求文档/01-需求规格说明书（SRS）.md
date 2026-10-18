# InduRTDB 需求规格说明书（SRS）

**版本：3.1.0**
**日期：2026-08-15**
**修订说明**：v3.1.0 新增崩溃自愈（owner 崩溃接管 + 奇数 write_seq 恢复）需求；集成产品化（find_package / pkg-config / FetchContent 三种引入方式）；测试扩展至 26 套件 / 126 用例。
（v3.0.0：纯 C11 重写, C++ API 已移除; 编译器标准 C++17 → C11 (`-std=gnu11`). 新增 check_timeouts, API 26 函数.）

---

## 1. 数据模型

```c
typedef struct {
    union { bool b; int32_t i; double d; char str[32]; } value;
    uint8_t   type;         // BOOL=0, INT32=1, DOUBLE=2, STRING=3
    uint64_t  timestamp_ns;
    uint8_t   quality;      // GOOD=0, BAD=1, TIMEOUT=2, SUBSTITUTED=3
    uint16_t  unit;
    uint8_t   access;       // READ_ONLY=1, READ_WRITE=3
    char      name[64];
    uint8_t   padding[19];
} __attribute__((packed, aligned(128))) indurtdb_point_t;
```

> _Static_assert 保证 sizeof == 128, 与 v2.x 布局逐字节一致。

## 2. C API（90 个函数：v1 全局 47 + v2 句柄 43）

详见 `include/indurtdb/indurtdb.h`。

## 3. 非功能性需求

| 指标 | 要求 |
|------|------|
| 写入延迟 P99 | ≤ 10 μs |
| 读取延迟 P99 | ≤ 5 μs |
| 编译器 | GCC ≥7.5, C11 (`-std=gnu11`) |
| 可靠性 | 7×24 无泄漏; 僵尸订阅者自动清理(心跳>1s) |
| 安全性 | access 控制: 只读点位拒绝写入 |
| 测试覆盖率 | gtest 多套件（`ctest` 注册 **31 个**用例：多单元 + 多进程集成 + 端到端），100% 通过 |

## 4. 版本路线图

| 版本 | 日期 | 状态 |
|------|------|------|
| v1.0.0 | 2026-03-27 | ✅ |
| v2.0.0 | 2026-05-11 | ✅ |
| v2.1.0 | 2026-05-11 | ✅ |
| v3.0.0 | 2026-07-21 | ✅ 纯 C11 重写 |
| … | … | 见 [`05-技术演进规划.md`](../06-开发规划/05-技术演进规划.md) |
| **v3.9** | — | 🔜 **点位分区 / 命名空间**（需求登记见白皮书 §8；方案设计见 [`08-v3.9-点位分区与命名空间方案设计.md`](../03-设计文档/08-v3.9-点位分区与命名空间方案设计.md)） |

## 5. v3.9 需求增量（点位分区 / 命名空间）

> 本节为增量登记。**权威需求与验收以 [`03-设计文档/08-v3.9-点位分区与命名空间方案设计.md`](../03-设计文档/08-v3.9-点位分区与命名空间方案设计.md) §2 为准**（该文已于 2026-10-18 方案评审通过；任务分解见 [`14-v3.9任务规划.md`](../06-开发规划/14-v3.9任务规划.md)）。

| 编号 | 需求 | 说明 |
|---|---|---|
| R1 | 点位可归属分区 | 新增 `uint32 partition`（默认 0）；`sizeof(indurtdb_point_t)` 与既有偏移**不变** |
| R2 | 名称在分区内唯一 | name→id 索引引入分区维度；同名点可在不同分区共存 |
| R3 | 按分区选择 | 查找 / 注册 / 重命名 / 删除 / 列举可限定分区 |
| R4 | 策略可限定分区 | 策略行由 3 段向后兼容扩为可选 4 段 `<uid>:<partition>:<min_id>:<max_id>` |
| R5 | 协议 / CLI / 监控台暴露分区 | 协议主版本 2 → 3；`irtcli` `--partition`/`select`；监控台 `partitions` 视图 |
| R6 | 迁移 | 段版本 2 → 3；`tools/irt_migrate` 扩展 v2→v3（`partition` 默认 0） |
| R7 | 默认分区行为等价 | `partition == 0` 语义等价 v3.8（除版本协商拒绝旧段外） |

**非目标**：真·独立 per-partition id 空间（→ v4.0）、分区作为安全边界、分区级配额/增删 API、点位跨分区移动。
