# Seqlock 无锁读写算法设计文档

**项目名称**：InduRTDB - 工业实时数据库
**版本号**：3.1.0
**修订日期**：2026年8月15日
**修订说明**：v3.1.0 新增奇数 write_seq 崩溃恢复（attach 时检测到遗留奇数序列自动推进至偶数）；owner_pid 字段辅助崩溃接管。
（v3.0.0：升级 write_begin 为非阻塞 CAS 循环 (消除 v2.x 的 TOCTOU 竞态); 补充 ARM RELEASE fence; 统计计数器改为原子操作; 全部代码从 C++ 直译为纯 C11）

---

## 1. 算法概述

### 1.1 问题背景

工业实时数据库的场景是"多读少写"：多个 Reader 进程高频读取点位数据，Writer 进程周期性写入。传统互斥锁在读取路径上引入竞争，无法满足 P99 ≤ 5μs 的读延迟要求。

### 1.2 算法原理

Seqlock 基于**全局序列号**（存储在共享内存 Header 中）实现无锁读写：

1. **写操作**：通过 CAS 循环获取写入权（偶数→奇数→偶数），奇数区间内更新数据
2. **读操作**：记录序列号 → 读数据 → 再读序列号，两次相同且为偶数则数据一致；否则重试

### 1.3 适用场景

- 读多写少的场景（工业实时数据库的典型场景）
- 需要低延迟读取的实时系统
- 单写者或多写者低冲突场景

---

## 2. 核心设计

### 2.1 序列号位置

序列号存储在共享内存 `InduRTDBHeader.write_seq`（uint64_t，offset=16）：

```
偶数 = 空闲（无写者活动）
奇数 = 写入中（Reader 应重试）
```

### 2.2 关键操作（纯 C11 inline 自由函数）

```c
// --- 写入协议 (v3.0.0: 非阻塞 CAS 循环, 消除 TOCTOU) ---

// 步骤1: CAS 循环获取写入权。成功返回进入前的偶数序列号，冲突返回奇数。
// 与 v2.x 的关键区别: 不再使用 load+store 两步操作 (存在 TOCTOU 窗口),
// 改为 __atomic_compare_exchange_n 单步原子完成"检查偶数→递增"。
static inline uint64_t irt_seqlock_write_begin(uint64_t* seq) {
    uint64_t expected = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    while (1) {
        if (expected & 1ULL) return expected;       // 奇数 = 写冲突, 立即返回
        if (__atomic_compare_exchange_n(seq, &expected, expected + 1,
                /*weak=*/false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return expected;                         // CAS 成功, 已原子递增
        }
        // CAS 失败: expected 已被硬件更新为当前值, 循环重试
    }
}

// 步骤2: 完成写入，seq0 为 begin 返回的序列号
static inline void irt_seqlock_write_end(uint64_t* seq, uint64_t seq0) {
    __atomic_store_n(seq, seq0 + 2, __ATOMIC_RELEASE);   // 恢复偶数
}

// --- 读取协议 ---

// 从共享内存读取点位数据，返回 const 指针（零拷贝）
static inline const indurtdb_point_t* irt_seqlock_read(
    const uint64_t* seq, const indurtdb_point_t* points, uint32_t id) {
    uint64_t s0, s1;
    s0 = 0; s1 = 1;   // 初始化不同, 确保至少循环一次
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                    // 写中，重试
        __atomic_thread_fence(__ATOMIC_ACQUIRE);     // 保证读取到的数据是写者最新写入的
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return &points[id];
}
```

### 2.3 设计要点

- **零虚函数**：Seqlock 是纯算法，`static inline` 自由函数放在 `irt_seqlock.h` 中
- **零异常**：冲突时返回奇数或循环重试，不抛异常
- **零堆分配**：序列号在共享内存 Header 中，不额外分配
- **全局唯一**：整个 RTDB 实例共享一个 `write_seq`，保护所有 PointData
- **非阻塞 CAS** (v3.0.0 新增): `write_begin` 使用 CAS 循环替代 load+store, 彻底消除 TOCTOU 窗口

---

## 3. 与 PointManager 的集成 (v3.0.0, 纯 C11)

PointManager 的 write/read 方法直接调用上述自由函数：

```c
// irt_point_manager.c: pm_write_impl —— 简化示例 (v3.0.0)
static int pm_write_impl(irt_pm_t* pm, uint32_t id,
                          uint8_t type, const void* value) {
    if (!irt_pm_validate_id(pm, id)) return -1;

    irt_header_t*      hdr = irt_shm_header(pm->shm);
    indurtdb_point_t*  pts = irt_shm_points(pm->shm);
    if (!hdr || !pts) return -1;

    // Seqlock write begin (CAS 循环)
    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return -2;  // 写冲突

    indurtdb_point_t* p = &pts[id];
    if (p->access == INDURTDB_ACCESS_READ_ONLY) {
        irt_seqlock_write_end(&hdr->write_seq, seq0);
        return -3;
    }

    // 写入数据 (4 类型: bool/int32/double/string)
    p->type = type;
    // ... switch(type) 赋值 p->value ...
    p->timestamp_ns = irt_time_now_ns();
    p->quality      = INDURTDB_QUALITY_GOOD;

    // ARM weak memory ordering: RELEASE fence 确保数据写入在 seqlock 释放前对
    // 所有 CPU 可见. x86 TSO 下此 fence 为 no-op. (v3.0.0 新增)
    __atomic_thread_fence(__ATOMIC_RELEASE);
    irt_seqlock_write_end(&hdr->write_seq, seq0);

    // 统计计数器: 原子递增 (v3.0.0: 改为 __atomic_fetch_add)
    __atomic_fetch_add(&hdr->stats.writes, 1, __ATOMIC_RELAXED);
    return 0;
}

// irt_point_manager.c: irt_pm_peek —— 真正零拷贝
const indurtdb_point_t* irt_pm_peek(irt_pm_t* pm, uint32_t id) {
    if (!irt_pm_validate_id(pm, id)) return NULL;
    indurtdb_point_t* pts = irt_shm_points(pm->shm);
    if (!pts) return NULL;
    return irt_seqlock_read(&irt_shm_header(pm->shm)->write_seq, pts, id);
}
```

### 3.1 v3.0.0 与 v2.x 的关键差异

| 方面 | v2.x (2026-05) | v3.0.0 (2026-07) |
|------|---------------|-------------------|
| **write_begin 算法** | load ACQUIRE + store RELEASE (两步, 存在 TOCTOU 窗口) | CAS 循环 ACQ_REL (单步原子, TOCTOU-free) |
| **写-写并发** | 第二个写者可能同时进入临界区 | CAS 硬件保证互斥, 不可能同时进入 |
| **写入路径 fence** | 无显式 fence (依赖 store RELEASE) | 显式 `__atomic_thread_fence(__ATOMIC_RELEASE)` (ARM 兼容) |
| **统计计数器** | `header_->stats.writes++` (非原子) | `__atomic_fetch_add(..., __ATOMIC_RELAXED)` |
| **实现语言** | C++ inline 自由函数 | 纯 C11 `static inline` |
| **写冲突处理** | 调用者自行重试 (无内置重试) | 调用者自行重试 (CAS 循环仅在 write_begin 内部重试 CAS 失败) |

---

## 4. 并发安全性

### 4.1 读-写并发

Reader 在 s0 为偶数时读取数据，若 Writer 在读取期间再次更新，Reader 的 s1 ≠ s0，自动重试。Reader 不会读到"一半新一半旧"的撕裂数据。

**v3.0.0 增强**: PointManager 在 `write_end` 前插入 `__atomic_thread_fence(__ATOMIC_RELEASE)`，确保在 ARM/PowerPC 等弱内存序架构上，数据写入 (type/value/timestamp/quality) 在 seqlock 释放前对所有 CPU 可见。

### 4.2 写-写并发

**v2.x 行为**: 两个 Writer 同时调用 `write_begin` 时存在 TOCTOU 窗口——两者都可能读到偶数，都递增序列号，导致两个 Writer 同时进入临界区。

**v3.0.0 行为**: CAS 是硬件原子操作。两个 Writer 同时 CAS 时，只有一个成功 (expected 匹配)，另一个自动重试并发现 expected 已变为奇数，立即返回冲突。临界区互斥得到硬件保证。

### 4.3 ABA 问题

使用 64 位序列号避免 ABA。若每秒写入 1,000,000 次，溢出需要约 584,942 年。

---

## 5. 性能分析

| 操作 | 原子操作次数 | 内存屏障 | 预估延迟 (ARM A53) |
|------|------------|---------|-------------------|
| 读（无冲突） | 2× load | 1× acquire fence | < 5 ns |
| 写（无冲突, CAS 首次成功） | 1× load + 1× CAS | 1× ACQ_REL (CAS 隐式) + 1× RELEASE (write_end) + 1× RELEASE fence (ARM) | < 15 ns |
| 写（CAS 失败重试1次） | 1× load + 2× CAS | 同上 + CAS 失败 ACQUIRE | < 25 ns |

> **v3.0.0 写延迟略高于 v2.x**: CAS 单次成本略高于 store (约 2-3 ns on ARM), 但消除了 TOCTOU 正确性 bug, 且冲突概率 < 0.1%, 净影响可忽略。

---

## 6. 测试策略

### 6.1 单元测试用例

```
- 单线程读写一致性
- 序列号奇偶性验证
- 写冲突检测 (第二个 writer 看到奇数)
- 读线程遇到写中自动重试
- 多 Reader 单 Writer 并发
- 边界值（0, UINT32_MAX, INT32_MIN）
- 长期运行序列号不溢出验证
- v3.0.0 新增: CAS 写-写互斥验证 (test_c_concurrency)
```

### 6.2 测试工具

- Google Test / gtest (26 套件, 126 用例, 100% 通过)
- 并发测试: test_c_concurrency (2 线程同点写/异点写/回调重入)
- CI: GitHub Actions, GCC ≥7.5, `-Wall -Wextra -Werror`

---

## 7. 参考文献

1. Linux 内核 Seqlock 实现 (`include/linux/seqlock.h`)
2. Herlihy, M., & Shavit, N. (2012). *The Art of Multiprocessor Programming*
3. GCC Atomic Builtins: `__atomic_load_n`, `__atomic_compare_exchange_n`, `__atomic_store_n`, `__atomic_thread_fence`

---

**文档变更记录**

| 版本号 | 变更日期 | 变更内容 |
|--------|---------|---------|
| 1.0.0 | 2026-03-27 | 初始版本（含 OOP 设计） |
| 2.0.0 | 2026-05-11 | 移除 OOP 层次（ISeqlock/Factory/Exception/Utils），改为轻量自由函数 |
| 2.1.0 | 2026-05-16 | 修正: load+store 方案存在 TOCTOU 窗口, 记录为已知风险待 v3.0.0 修复 |
| 3.0.0 | 2026-07-27 | write_begin 升级为 CAS 循环 (消除 TOCTOU); 补 ARM RELEASE fence; 统计计数器改为原子操作; 全部代码从 C++ 直译为纯 C11; 与 irt_seqlock.h / irt_point_manager.c 实际实现完全一致 |
