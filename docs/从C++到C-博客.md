# 从 C++ 到 C：一个工业实时数据库的瘦身之旅

> 我们删掉了 5000 行代码，新增了 1000 行，功能反而更多了。这不是魔法，只是把 C++ 的抽象税退了。

---

## 背景

InduRTDB 是一个面向工业边缘控制场景的实时数据库。它的核心需求很简单：

- 多个进程共享同一块内存区域
- 低延迟读写点位数据（bool/int32/double/string）
- 订阅点位的变更通知
- 7x24 小时稳定运行，不能有内存碎片

v2.1.0 用 C++17 实现。代码量约 3900 行（不含测试），包含了完整的单元测试和集成测试。功能齐全，测试通过，看起来没什么问题。

但当我们把目光投向 SylixOS（一个国产实时操作系统）时，问题开始浮现。

---

## C++ 的代价

### 1. SylixOS 不支持完整的 C++17

SylixOS 的 C++ 运行时库覆盖不完整。特别是 `stdatomic.h`——C++17 的原子操作标准库——在 SylixOS 上行为不可预期。而我们整个 Seqlock 无锁读写机制都依赖原子操作。

怎么办？我们开始用 GCC 的 `__atomic_*` builtins 替代 `std::atomic`。改了 Seqlock，改了 PointManager，改了 SubscriptionManager……越改越多，越改越深。

### 2. 抽象层级过多

v2.1.0 有一个看起来很漂亮的分层架构：

```
include/indurtdb/
├── api/
│   ├── indurtdb.hpp        # C++ 模板 API
│   └── c/indurtdb_c.h      # C ABI 桥接层
├── core/
│   ├── seqlock.hpp                 # ISeqlock 虚接口
│   ├── point_manager_interface.hpp  # IPointManager 虚接口
│   ├── subscription_manager_interface.hpp  # ISubscription 虚接口
│   └── shared_memory_segment.hpp   # ISharedMemory 虚接口
├── osal/
│   ├── interface.hpp        # ITime/INotification/IThreading 虚接口
│   └── factory.hpp          # 对象工厂
└── types/
    ├── basic_types.hpp
    └── memory_layout.hpp
```

5 个虚接口，1 个工厂，1 个 PIMPL 实现类。在 Linux 上，这没问题——编译器优化会把虚函数调用的开销降到很低。

但在 ARM Cortex-A 上，每一个虚函数调用都是一次间接跳转，意味着 CPU 分支预测失败时的一次流水线刷新。对于要求 P99 <= 10us 的实时系统，这种不确定性是不可接受的。

### 3. 无用代码的堆积

打开 `src/utils/logging.cpp`，186 行。它是干什么的？包装了一下 `printf`，加了日志级别。

打开 `src/utils/error.cpp`，139 行。错误信息管理。

打开 `src/osal/posix/notification_posix.cpp`，131 行。Unix Domain Socket 实现，用于跨进程通知。但我们在嵌入式设备上，订阅通知完全可以用进程内的函数指针回调。

打开 `src/osal/posix/threading_posix.cpp`，44 行。封装了 `pthread_setaffinity_np` 和 `sched_yield`。然后打开 `src/api/cpp/indurtdb_impl.cpp`，259 行 PIMPL 转发……

这些东西加起来，1300 多行代码，没有一行是产品逻辑。

---

## 决策：一不做二不休

与其修修补补让 C++17 在 SylixOS 上勉强能跑，不如从头用 C 重写。

但有两条红线不能碰：

1. **共享内存布局必须逐字节兼容。** 已经在现场运行的系统不能因为这次重写而丢数据。
2. **所有现有功能必须保留。** 这不是精简版，是完整重写。

我们拉了一个新分支 `feature/pure-c-rewrite`，做了 9 个 task 的拆分。

---

## 重写策略：直译而非重构

所谓"直译"，就是把 C++ 的类和模板，逐结构体、逐函数地翻译成 C。算法不改，数据布局不改，只去掉 C++ 特有的语法糖:

| C++ | C |
|-----|---|
| 虚接口 `class IPointManager` | 结构体 `irt_pm_t` + 自由函数 `irt_pm_write_double()` |
| `template<typename T> bool write(PointId, const T&)` | 4 个显式函数：`write_bool/int32/double/string` |
| `std::function<...>` 回调 | C 函数指针 `typedef void (*indurtdb_callback_t)(...)` |
| `std::vector`/`std::unordered_map` | 定长数组 `slots[256]` |
| `std::mutex` | `__atomic_*` builtins + Seqlock |
| `_Thread_local` (仅一处) | 纯 C11 的 thread-local storage |
| `PIMPL` + static local singleton | `static irt_globals_t g_rtdb` |
| C ABI 桥接层 (116 行) | **不需要** —— C API 是原生的 |

关键原则：**"一个头文件 + 六个 .c 文件 + 一个 OSAL 层，编译出 libindurtdb.a"**。用户只需要 `#include <indurtdb/indurtdb.h>`。

---

## 瘦身结果

```
                    C++ v2.1.0      C v3.0.0       变化
─────────────────────────────────────────────────────────
公共头文件           1 个聚合头         1 个 indurtdb.h   -400 行
                   + 12 个子头
核心源文件           6 个 .cpp        6 个 .c          -650 行
API 实现             375 行           230 行           -145 行
  (含 C ABI 桥接)   (C++ 259 + C 116)
OSAL                 432 行           195 行           -237 行
  每个平台          4 个 .cpp         1 个 .c
构建文件             8 个 CMakeLists   1 个             -300 行
─────────────────────────────────────────────────────────
合计                 ~3900 行          ~1160 行        -70%
```

54 个文件变成 16 个。5395 行被删掉，3427 行被写入。

但更重要的是功能：

| 功能 | C++ v2.1.0 | C v3.0.0 |
|------|:----------:|:--------:|
| 单点读写 (4 类型) | Yes | Yes |
| 零拷贝 peek | Yes (仅 C++) | Yes (C 可用) |
| 点位订阅/取消 | Yes (仅 C++) | Yes (C 可用) |
| 配置加载 | Yes (仅 C++) | Yes (C 可用) |
| 心跳/僵尸清理 | Yes (仅 C++) | Yes (C 可用) |
| 状态查询 | Yes | Yes |
| **批量读写 (range)** | **No** | **Yes** |
| **线程安全错误信息** | No | **Yes** (_Thread_local) |

旧的 C API 只有 14 个函数。新的 `indurtdb.h` 有 26 个，而且所有函数对 C 来说是原生的——C++ 用户通过 `extern "C"` 调用即可。

**功能多了，代码少了。** 这违反直觉，但如果你见过 C++ 项目里虚接口 + 工厂 + PIMPL + STL + C ABI 桥接的组合拳，你就理解为什么。

---

## 一个具体的例子

这是 v2.1.0 的 C++ 写入接口（简化版）：

```cpp
// include/indurtdb/api/indurtdb.hpp
template<typename T>
bool write(PointId id, const T& value);

// src/api/cpp/indurtdb_impl.cpp
bool InduRTDB::Impl::write<double>(PointId id, const double& value) {
    return point_manager_->write(id, value);
}

// src/core/point_manager.cpp (虚函数分发)
bool PointManagerImpl::write<double>(PointId id, const double& value) {
    auto* p = &points_[id];
    // Seqlock begin/end + 写入逻辑 ...
}

// src/api/c/indurtdb_c_impl.cpp (C ABI 桥接)
int indurtdb_write_double(uint32_t id, double value) {
    return InduRTDB::instance().write(id, value) ? 0 : -1;
}
```

四层调用链：C ABI -> C++ API -> PIMPL -> 虚接口 -> 实现。

v3.0.0 的 C 版本：

```c
// include/indurtdb/indurtdb.h
int indurtdb_write_double(uint32_t id, double value);

// src/api/indurtdb.c
int indurtdb_write_double(uint32_t id, double value) {
    if (!g_rtdb.initialized) return IRT_ERR_NOT_INIT;
    return irt_pm_write_double(&g_rtdb.pm, id, value);
}

// src/core/irt_point_manager.c
int irt_pm_write_double(irt_pm_t* pm, uint32_t id, double value) {
    irt_point_t* p = irt_pm_point_at(pm, id);
    if (!p) return IRT_ERR_INVALID_ID;
    // Seqlock begin/end + 写入逻辑 ...
}
```

两层调用链。没有模板展开，没有虚表查找，没有 `extern "C"` 包装。调试器可以直接在任意一层打断点，不会迷失在模板实例化栈里。

---

## 测试驱动重写

重写过程中，我们保持了 8 个 GTest 测试（用 C++17 编译，但测试的是 C 接口）：

```
100% tests passed, 0 tests failed out of 8

test_c_api                — 公共 API 26/26 函数全覆盖
test_c_config             — key=value 配置解析
test_c_layout_seqlock     — 共享内存布局 + Seqlock 正确性
test_c_osal               — OS 抽象层 (shm_open/mmap/时间)
test_c_pm                 — 点位管理器 (4 类型读写 + peek)
test_c_shm                — 共享内存段 (owner/magic/version)
test_c_sub                — 订阅 (注册/通知/心跳/僵尸清理)
test_c_multi_process      — 多进程 fork + 原始布局字节回归
```

第 8 个测试是关键的回归测试——它直接 `fork()`，父子进程分别读写同一块共享内存，然后逐字节对比共享内存的原始布局，确保与 v2.x 完全一致。

这 8 个测试贯穿了整个 9 个 task 的开发，每一步提交都必须全部通过。

---

## 启示

1. **C11 对于系统级库来说完全够用。** `_Thread_local`、`__atomic_*` builtins、匿名 union、`_Static_assert`——这些 C11 特性写出来的代码和 C++ 一样安全，而且没有隐式运行时依赖。

2. **每个抽象都应该付费。** 虚接口不是免费的——它增加了理解成本、调试困难度和移植风险。如果不需要运行时多态，就不要引入虚函数。

3. **嵌入式平台是 C++ 的照妖镜。** 在 Linux x86_64 上什么事都没有，换到 ARM + SylixOS，模板展开的二进制膨胀、虚表的间接跳转延迟、STL 容器的内存碎片，全都暴露出来。

4. **代码行数不是生产力指标。** 删除 5000 行、新增 1000 行、净减 4000 行——这个 diff 看起来像一次大规模的"破坏"，但实际上功能反而增加了。**花时间写更少的代码，才是真正的工程。**

---

## 后续

InduRTDB v3.0.0 已合入 main 分支，以 git submodule 的方式集成到 m580cn vPLC 项目中。接下来：

- SylixOS ARM Cortex-A53 目标硬件上的性能验证（P99 延迟测量）
- 如果 rtdb_server (C++ 服务) 需要使用 InduRTDB，它只需 `#include <indurtdb/indurtdb.h>` + `extern "C"` 即可
- 考虑提供 Python ctypes 绑定——因为 C API 不需要 SWIG 或 pybind11，直接用 `ctypes.CDLL` 就能加载

---

> **代码可以重写，设计可以迭代，但共享内存布局一旦定义就是承诺。**
>
> v3.0.0 的 `irt_types.h` 里，Header 依然是 64 字节，PointData 依然是 128 字节，SubscriberEntry 依然是 16 字节，magic 依然是 `0x1DBA1DBA`。
>
> 这份承诺，用 C 写比用 C++ 写可靠得多。

---

*2026 年 7 月 23 日 | InduRTDB Team | [GitHub](https://github.com/turnarond/InduRTDB)*
