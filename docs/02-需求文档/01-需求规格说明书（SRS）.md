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

## 2. C API (26 个函数)

详见 `include/indurtdb/indurtdb.h`。

## 3. 非功能性需求

| 指标 | 要求 |
|------|------|
| 写入延迟 P99 | ≤ 10 μs |
| 读取延迟 P99 | ≤ 5 μs |
| 编译器 | GCC ≥7.5, C11 (`-std=gnu11`) |
| 可靠性 | 7×24 无泄漏; 僵尸订阅者自动清理(心跳>1s) |
| 安全性 | access 控制: 只读点位拒绝写入 |
| 测试覆盖率 | 26 个测试套件, 126 用例, 100% 通过 |

## 4. 版本路线图

| 版本 | 日期 | 状态 |
|------|------|------|
| v1.0.0 | 2026-03-27 | ✅ |
| v2.0.0 | 2026-05-11 | ✅ |
| v2.1.0 | 2026-05-11 | ✅ |
| v3.0.0 | 2026-07-21 | ✅ 纯 C11 重写 |
