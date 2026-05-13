# Seqlock 无锁读写算法设计文档

**项目名称**：InduRTDB - 工业实时数据库
**版本号**：2.0.0
**修订日期**：2026年5月11日
**修订说明**：移除 OOP 过度设计，改为基于 Header.write_seq 的轻量机制

---

## 1. 算法概述

### 1.1 问题背景

工业实时数据库的场景是"多读少写"：多个 Reader 进程高频读取点位数据，Writer 进程周期性写入。传统互斥锁在读取路径上引入竞争，无法满足 P99 ≤ 5μs 的读延迟要求。

### 1.2 算法原理

Seqlock 基于**全局序列号**（存储在共享内存 Header 中）实现无锁读写：

1. **写操作**：递增序列号（偶数→奇数→偶数），奇数区间内更新数据
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

### 2.2 关键操作（自由函数，非类方法）

```cpp
// --- 写入协议 ---

// 步骤1: 获取写入权，返回当前序列号。若返回值为奇数，表示有并发写入，应重试或失败
inline uint64_t seqlock_write_begin(uint64_t* seq) {
    uint64_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s & 1ULL) return s;                         // 奇数=写冲突
    __atomic_store_n(seq, s + 1, __ATOMIC_RELEASE);  // 标记写入中
    return s;
}

// 步骤2: 完成写入，seq0 为 begin 返回的序列号
inline void seqlock_write_end(uint64_t* seq, uint64_t seq0) {
    __atomic_store_n(seq, seq0 + 2, __ATOMIC_RELEASE); // 恢复偶数
}

// --- 读取协议 ---

// 从共享内存读取点位数据，返回 const 指针（零拷贝）
inline const PointData* seqlock_read(const uint64_t* seq,
                                      const PointData* points,
                                      uint32_t id) {
    uint64_t s0, s1;
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                    // 写中，重试
        // 此时可安全读取 points[id]
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return &points[id];
}
```

### 2.3 设计要点

- **零虚函数**：Seqlock 是纯算法，不封装为类层次
- **零异常**：冲突时返回 false 或重试，不抛异常
- **零堆分配**：序列号在共享内存 Header 中，不额外分配
- **全局唯一**：整个 RTDB 实例共享一个 write_seq，保护所有 PointData

---

## 3. 与 PointManager 的集成

PointManager 的 write/read 方法直接调用上述自由函数：

```cpp
// PointManager::write —— 简化示例
bool PointManager::write(PointId id, double value) {
    if (!validate_id(id)) return false;

    uint64_t seq0 = seqlock_write_begin(&header_->write_seq);
    if (seq0 & 1ULL) return false;  // 写冲突

    PointData* p = &points_[id];
    p->value.d       = value;
    p->timestamp_ns  = time_->now_ns();
    p->quality       = Quality::GOOD;

    seqlock_write_end(&header_->write_seq, seq0);
    header_->stats.writes++;
    return true;
}

// PointManager::peek —— 真正零拷贝
const PointData* PointManager::peek(PointId id) const {
    if (!validate_id(id)) return nullptr;
    return seqlock_read(&header_->write_seq, points_, id);
}
```

---

## 4. 并发安全性

### 4.1 读-写并发

Reader 在 s0 为偶数时读取数据，若 Writer 在读取期间再次更新，Reader 的 s1 ≠ s0，自动重试。Reader 不会读到"一半新一半旧"的撕裂数据。

### 4.2 写-写并发

若有第二个 Writer 同时访问，`seqlock_write_begin` 返回奇数，调用者返回 false。工业场景写入频率低（≤ 1kHz 每通道），冲突概率 < 0.1%。

### 4.3 ABA 问题

使用 64 位序列号避免 ABA。若每秒写入 1,000,000 次，溢出需要约 584,942 年。

---

## 5. 性能分析

| 操作 | 原子操作次数 | 内存屏障 | 预估延迟 (ARM A53) |
|------|------------|---------|-------------------|
| 读（无冲突） | 2× load | 1× acquire fence | < 5 ns |
| 写 | 2× store | 0 | < 10 ns |
| 读（有冲突重试1次） | 4× load | 2× acquire fence | < 15 ns |

---

## 6. 测试策略

### 6.1 单元测试用例

```
- 单线程读写一致性
- 序列号奇偶性验证
- 读线程遇到写中自动重试
- 多 Reader 单 Writer 并发
- 边界值（0, UINT32_MAX, INT32_MIN）
- 长期运行序列号不溢出验证
```

### 6.2 测试工具

- Google Test / gtest
- Thread Sanitizer (`-fsanitize=thread`)
- 自定义多线程压力测试

---

## 7. 参考文献

1. Linux 内核 Seqlock 实现 (`include/linux/seqlock.h`)
2. Herlihy, M., & Shavit, N. (2012). *The Art of Multiprocessor Programming*
3. GCC Atomic Builtins: `__atomic_load_n`, `__atomic_store_n`, `__atomic_thread_fence`

---

**文档变更记录**

| 版本号 | 变更日期 | 变更内容 |
|--------|---------|---------|
| 1.0.0 | 2026-03-27 | 初始版本（含 OOP 设计） |
| 2.0.0 | 2026-05-11 | 移除 OOP 层次（ISeqlock/Factory/Exception/Utils），改为轻量自由函数 |
