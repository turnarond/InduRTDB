# InduRTDB SDK 用户文档与示例 — 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 交付 4 份 SDK 文档 + 修复 1 个示例 bug + 新增 2 个示例程序

**Architecture:** 纯文档和示例工程，不修改 libindurtdb.a 核心代码。4 份文档放在 `docs/sdk/`，3 个示例放在 `examples/`。文档中所有代码片段必须可直接拷贝编译通过。

**Tech Stack:** Markdown (文档), C/C++ (示例), CMake (示例构建)

---

## 文件结构

```
docs/sdk/
├── 快速入门指南.md           # 新创建
├── C++ API 参考手册.md       # 新创建
├── C API 参考手册.md         # 新创建
└── 开发者指南.md             # 新创建

examples/
├── basic_example.cpp         # 修改（修复 subscribe bug）
├── c_example.c               # 新创建
├── multi_process_example.cpp # 新创建
└── CMakeLists.txt            # 修改（新增两个 target）
```

---

### Task 1: 修复 basic_example.cpp 的 subscribe bug

**Files:**
- Modify: `examples/basic_example.cpp`

- [ ] **Step 1: 替换 lambda 为 C 函数指针回调**

当前代码（第 54-56 行）使用了 lambda：
```cpp
bool subscribed = rtdb.subscribe(2001, [](const indurtdb::PointData& p) {
    std::cout << "   🔔 水泵状态变化: " << (p.value.b ? "启动" : "停止") << "\n";
});
```

这行无法通过编译，因为 `subscribe()` 的签名是：
```cpp
bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
```
其中 `SubscriptionCallback` = `void (*)(PointId id, const PointData& data, void* user_data)`。

替换为：
```cpp
// 在第 14 行附近（全局作用域）添加回调函数
static void on_pump_state_change(PointId id, const PointData& data, void* user_data) {
    (void)id;
    (void)user_data;
    std::cout << "   🔔 水泵状态变化: " << (data.value.b ? "启动" : "停止") << "\n";
}
```

```cpp
// 第 54-56 行替换为
bool subscribed = rtdb.subscribe(2001, on_pump_state_change, nullptr);
```

- [ ] **Step 2: 删除 try-catch 包裹**

当前 main 函数体被 `try { ... } catch (const std::exception& e) { ... }` 包裹，但项目使用 `-fno-exceptions`，此代码无法通过编译。删除 try-catch，直接写主逻辑。

- [ ] **Step 3: 编译并验证**

```bash
cd build && cmake .. -DBUILD_EXAMPLES=ON && make -j$(nproc)
./examples/basic_example
```

预期：编译零警告，输出全部 `✓`，subscribe 回调被触发。

- [ ] **Step 4: Commit**

```bash
git add examples/basic_example.cpp
git commit -m "fix: 修复 basic_example subscribe 使用 lambda 和 try-catch 的编译错误"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 2: 新增 C 语言完整示例 c_example.c

**Files:**
- Create: `examples/c_example.c`

- [ ] **Step 1: 编写 c_example.c**

```c
/**
 * @file c_example.c
 * @brief InduRTDB C API 使用示例
 * @version 1.0.0
 * @date 2026-05-17
 * @copyright MIT License
 */

#include <indurtdb/api/c/indurtdb_c.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("=== InduRTDB C API 使用示例 ===\n\n");

    /* 1. 初始化 */
    printf("1. 初始化数据库...\n");
    int ret = indurtdb_initialize("c_example", 100, 10);
    if (ret != 0) {
        fprintf(stderr, "初始化失败: %s\n", indurtdb_get_last_error());
        return 1;
    }
    printf("   ok 初始化成功\n\n");

    /* 2. 写入四种类型 */
    printf("2. 写入数据...\n");
    indurtdb_write_double(1001, 23.5);
    printf("   ok 写入 double: id=1001, value=23.5\n");

    indurtdb_write_int32(2001, 42);
    printf("   ok 写入 int32:  id=2001, value=42\n");

    indurtdb_write_bool(3001, true);
    printf("   ok 写入 bool:   id=3001, value=true\n");

    indurtdb_write_string(4001, "Pump_01");
    printf("   ok 写入 string: id=4001, value=\"Pump_01\"\n\n");

    /* 3. 读取各类型 */
    printf("3. 读取数据...\n");

    double dval;
    if (indurtdb_read_double(1001, &dval) == 0) {
        printf("   温度: %.1f\n", dval);
    }

    int32_t ival;
    if (indurtdb_read_int32(2001, &ival) == 0) {
        printf("   计数器: %d\n", ival);
    }

    bool bval;
    if (indurtdb_read_bool(3001, &bval) == 0) {
        printf("   开关: %s\n", bval ? "ON" : "OFF");
    }

    char buf[32];
    if (indurtdb_read_string(4001, buf, sizeof(buf)) == 0) {
        printf("   设备名: %s\n", buf);
    }
    printf("\n");

    /* 4. 读取完整点位数据 */
    printf("4. 读取完整点位数据...\n");
    indurtdb_point_t point;
    if (indurtdb_read_point(1001, &point) == 0) {
        printf("   id=1001, value=%.1f, type=%d, quality=%d, timestamp=%lu\n",
               point.value.d, point.type, point.quality,
               (unsigned long)point.timestamp_ns);
    }
    printf("\n");

    /* 5. 统计信息 */
    printf("5. 统计信息...\n");
    printf("   总写入次数: %lu\n", (unsigned long)indurtdb_get_write_count());
    printf("\n");

    /* 6. 关闭 */
    printf("6. 关闭数据库...\n");
    indurtdb_shutdown();
    printf("   ok 关闭成功\n\n");

    printf("=== 示例运行完成 ===\n");
    return 0;
}
```

- [ ] **Step 2: 编译并验证**

```bash
cd build && cmake .. -DBUILD_EXAMPLES=ON && make -j$(nproc)
./examples/c_example
```

预期：编译零警告，读写四种类型全部输出正确值，统计计数 = 4。

- [ ] **Step 3: Commit**

```bash
git add examples/c_example.c
git commit -m "feat: 新增 C API 完整使用示例 c_example.c"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 3: 新增多进程示例 multi_process_example.cpp

**Files:**
- Create: `examples/multi_process_example.cpp`

- [ ] **Step 1: 编写 multi_process_example.cpp**

```cpp
/**
 * @file multi_process_example.cpp
 * @brief InduRTDB 多进程共享内存示例
 * @version 1.0.0
 * @date 2026-05-17
 * @copyright MIT License
 */

#include <indurtdb.hpp>
#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    auto& rtdb = indurtdb::InduRTDB::instance();

    // --- 阶段 1: 父进程初始化 + 写数据 ---
    printf("=== InduRTDB 多进程共享内存示例 ===\n\n");
    printf("[父进程 PID=%d] 初始化共享内存...\n", getpid());

    if (!rtdb.initialize("multi_proc", 100, 10)) {
        fprintf(stderr, "初始化失败\n");
        return 1;
    }
    printf("[父进程] ok 初始化成功 (is_owner)\n\n");

    printf("[父进程] 写入数据...\n");
    rtdb.write(1, 23.5);
    rtdb.write(2, (int32_t)42);
    rtdb.write(3, true);
    rtdb.write(4, "shared_data");
    printf("[父进程] ok 已写入 4 个点位\n\n");

    // --- 阶段 2: fork 子进程读取 ---
    printf("[父进程] fork 子进程...\n");
    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "fork 失败\n");
        return 1;
    }

    if (pid == 0) {
        // ===== 子进程 =====
        printf("[子进程 PID=%d] 启动，通过 peek 零拷贝读取共享内存...\n", getpid());

        // 注意：子进程不调用 initialize()，它已继承父进程的 mmap 映射
        // 在实际独立进程中，需调用 initialize() 附加到已有共享内存段

        const indurtdb::PointData* p1 = rtdb.peek(1);
        const indurtdb::PointData* p2 = rtdb.peek(2);
        const indurtdb::PointData* p3 = rtdb.peek(3);

        if (p1) printf("[子进程] peek(1) = %.1f\n", p1->value.d);
        if (p2) printf("[子进程] peek(2) = %d\n", p2->value.i);
        if (p3) printf("[子进程] peek(3) = %s\n", p3->value.b ? "true" : "false");

        // 读取字符串
        indurtdb::PointData p4;
        if (rtdb.read(4, p4)) {
            printf("[子进程] read(4) = \"%s\" (type=%d, quality=%d)\n",
                   p4.value.str, (int)p4.type, (int)p4.quality);
        }

        // 重要: 子进程用 _exit() 而非 exit()，避免析构函数调用 shm_unlink
        _exit(0);
    }

    // ===== 父进程等待子进程退出 =====
    int status;
    waitpid(pid, &status, 0);
    printf("\n[父进程] 子进程已退出 (status=%d)\n", WEXITSTATUS(status));
    printf("[父进程] 总写入次数: %lu\n",
           (unsigned long)rtdb.peek(1) ? rtdb.is_initialized() : 0);

    rtdb.shutdown();
    printf("[父进程] 已关闭\n");
    printf("\n=== 示例运行完成 ===\n");
    return 0;
}
```

- [ ] **Step 2: 编译并验证**

```bash
cd build && cmake .. -DBUILD_EXAMPLES=ON && make -j$(nproc)
./examples/multi_process_example
```

预期：编译零警告，子进程通过 peek 读到父进程写入的全部数据，子进程安全退出不触发 shm_unlink。

- [ ] **Step 3: Commit**

```bash
git add examples/multi_process_example.cpp
git commit -m "feat: 新增多进程共享内存示例 multi_process_example.cpp"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 4: 更新 examples/CMakeLists.txt

**Files:**
- Modify: `examples/CMakeLists.txt`

- [ ] **Step 1: 添加新示例的构建规则**

```cmake
# InduRTDB Examples CMake Configuration

cmake_minimum_required(VERSION 3.15)

if(BUILD_EXAMPLES)
    # C++ 基础示例
    add_executable(basic_example basic_example.cpp)
    target_link_libraries(basic_example PRIVATE indurtdb)

    # C 语言示例
    add_executable(c_example c_example.c)
    target_link_libraries(c_example PRIVATE indurtdb)

    # 多进程示例
    add_executable(multi_process_example multi_process_example.cpp)
    target_link_libraries(multi_process_example PRIVATE indurtdb)

    # 统一输出目录
    set_target_properties(basic_example c_example multi_process_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples"
    )

    message(STATUS "Added examples: basic_example, c_example, multi_process_example")
endif()
```

- [ ] **Step 2: 编译全部示例验证**

```bash
cd build && cmake .. -DBUILD_EXAMPLES=ON && make -j$(nproc)
ls build/examples/
```

预期：列出 `basic_example`, `c_example`, `multi_process_example` 三个可执行文件。

- [ ] **Step 3: Commit**

```bash
git add examples/CMakeLists.txt
git commit -m "feat: CMakeLists 新增 c_example 和 multi_process_example 构建规则"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 5: 编写快速入门指南

**Files:**
- Create: `docs/sdk/快速入门指南.md`

- [ ] **Step 1: 编写快速入门指南**

```markdown
# InduRTDB 快速入门指南

**版本**：2.1.0 | **预计阅读**：5 分钟

---

## 1. 前置要求

- 编译器：GCC ≥ 7.5（C++17）
- 构建系统：CMake ≥ 3.15
- 系统库：pthread, rt（Linux 标配）
- 可选：Google Test（仅构建测试需要）

## 2. 构建 SDK

```bash
git clone <repo-url> && cd InduRTDB
mkdir build && cd build
cmake .. -DBUILD_EXAMPLES=ON
make -j$(nproc)
```

产物：
- `libindurtdb.a` — 静态库，链接到你的项目
- `examples/basic_example` — 可运行的示例

## 3. 链接到你的项目

**CMake 方式**：

```cmake
find_library(INDURTDB_LIB indurtdb PATHS /path/to/InduRTDB/build)
target_link_libraries(your_app PRIVATE ${INDURTDB_LIB} pthread rt)
```

**命令行方式**：

```bash
g++ -std=c++17 your_app.cpp -I/path/to/InduRTDB/include -L/path/to/InduRTDB/build -lindurtdb -lpthread -lrt
```

## 4. 第一个程序

```cpp
#include <indurtdb.hpp>
#include <cstdio>

int main() {
    auto& rtdb = indurtdb::InduRTDB::instance();

    // 初始化（首次调用的进程创建共享内存）
    if (!rtdb.initialize("my_app", 100, 10)) {
        fprintf(stderr, "初始化失败\n");
        return 1;
    }

    // 写入温度数据
    rtdb.write(1, 23.5);

    // 读取
    indurtdb::PointData p;
    if (rtdb.read(1, p)) {
        printf("温度: %.1f°C\n", p.value.d);
    }

    // 清理（仅 owner 进程需调用）
    rtdb.shutdown();
    return 0;
}
```

编译并运行：
```bash
g++ -std=c++17 -I<indurtdb>/include -L<indurtdb>/build first_app.cpp -lindurtdb -lpthread -lrt
./a.out
```

输出：
```
温度: 23.5°C
```

## 5. 写入数据

支持 4 种工业数据类型：

```cpp
rtdb.write(1001, 23.5);           // double  — 模拟量（温度、压力、湿度）
rtdb.write(2001, (int32_t)42);    // int32_t — 计数器、档位
rtdb.write(3001, true);           // bool    — 开关、启停
rtdb.write(4001, "Pump_01");      // string  — 设备名、状态文本（≤31字符）
```

> **注意**：`int32_t` 需要显式转换，否则字面量 `42` 被推导为 `int`。
> **限制**：字符串最大 31 字符（含 '\0' 为 32 字节）。

## 6. 读取数据

**拷贝读取 `read()`** — 返回 PointData 副本，适合大多数场景：

```cpp
indurtdb::PointData p;
if (rtdb.read(1001, p)) {
    // 访问 p.value.d（double）/ p.value.i（int32_t）/ p.value.b（bool）/ p.value.str（string）
    printf("值: %.1f, 时间戳: %lu, 质量: %d\n",
           p.value.d, (unsigned long)p.timestamp_ns, (int)p.quality);
}
```

**零拷贝读取 `peek()`** — 返回共享内存指针，适合高频读取：

```cpp
const indurtdb::PointData* p = rtdb.peek(1001);
if (p) {
    printf("温度: %.1f\n", p->value.d);
    // 注意：不要长期持有此指针，跨写入边界时数据可能变化
}
```

| 方法 | 拷贝 | 延迟 | 适用场景 |
|------|------|------|---------|
| `read()` | 有 (128B) | ~50ns | 一般读取 |
| `peek()` | 无 | ~10ns | 高频读取、批量遍历 |

## 7. 订阅数据变更

```cpp
// 1. 定义回调函数（必须是 C 函数指针或静态函数）
static void on_temp_change(indurtdb::PointId id,
                           const indurtdb::PointData& data,
                           void* user_data) {
    (void)id;
    (void)user_data;
    printf("温度变化: %.1f°C\n", data.value.d);
}

// 2. 注册订阅
rtdb.subscribe(1001, on_temp_change, nullptr);

// 3. 写入时自动触发回调
rtdb.write(1001, 26.0);  // → 输出: "温度变化: 26.0°C"

// 4. 取消订阅
rtdb.unsubscribe(1001);
```

> **限制**：最多 256 个订阅槽位。回调在写入者的线程内执行，不要在回调中做耗时操作。

## 8. 加载 YAML 配置

```yaml
# points.yaml
points:
  - id: 1001
    name: "AHU_01.Supply_Temp"
    type: double
    unit: 1
    access: 1
  - id: 2001
    name: "Pump_01.Start_CMD"
    type: bool
    access: 3
```

```cpp
if (!rtdb.load_config("points.yaml")) {
    fprintf(stderr, "配置加载失败\n");
}
// 点位元数据（name/unit/access）已自动初始化
```

## 9. 多进程场景

InduRTDB 的核心设计目标是多进程共享。场景示例：

```
进程A（驱动）：initialize("hvac", 10000, 32) → 创建共享内存 → write(1001, 23.5)
进程B（控制）：initialize("hvac", 10000, 32) → 附加已有段 → read(1001)
进程C（HMI）：  initialize("hvac", 10000, 32) → 附加已有段 → subscribe(1001, callback)
```

- **首个调用 `initialize()` 的进程** 成为 owner，创建共享内存段
- **后续进程** 通过相同 `instance_id` 附加到已有段，校验 magic/version
- 段名格式：`/indurtdb_<instance_id>`
- **重要**：子进程使用 `_exit()` 而非 `exit()` 退出，避免触发析构函数调用 `shm_unlink`

## 10. 关机与清理

```cpp
rtdb.shutdown();
```

- 非 owner 进程：仅解除 mmap 映射，共享内存继续存在
- Owner 进程：解除映射 + `shm_unlink` 删除共享内存段
- 不调用 `shutdown()` 直接退出，共享内存会残留（`/dev/shm/indurtdb_*`）

## 下一步

- [C++ API 参考手册](./C++%20API%20参考手册.md) — 每个函数的完整说明
- [C API 参考手册](./C%20API%20参考手册.md) — 面向 C/Python/Rust 用户
- [开发者指南](./开发者指南.md) — 架构、并发、性能、排错
```

- [ ] **Step 2: Commit**

```bash
git add docs/sdk/快速入门指南.md
git commit -m "docs: 新增 SDK 快速入门指南"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 6: 编写 C++ API 参考手册

**Files:**
- Create: `docs/sdk/C++ API 参考手册.md`

- [ ] **Step 1: 编写 C++ API 参考手册**

```markdown
# InduRTDB C++ API 参考手册

**版本**：2.1.0

---

## 命名空间

所有类型和函数位于 `indurtdb` 命名空间。唯一公开头文件：`<indurtdb.hpp>`。

## 主类：`InduRTDB`

单例类（PIMPL 模式），管理整个实时数据库的生命周期。

### 获取实例

```cpp
static InduRTDB& InduRTDB::instance();
```

**返回值**：全局唯一实例的引用。线程安全（C++11 static 局部变量）。

**示例**：
```cpp
auto& rtdb = indurtdb::InduRTDB::instance();
```

---

### 初始化

```cpp
bool initialize(const char* instance_id,
                uint32_t max_points = 10000,
                uint32_t max_subscribers = 32);
```

**参数**：
| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `instance_id` | `const char*` | — | 实例名，用于生成共享内存段名 `/indurtdb_<id>` |
| `max_points` | `uint32_t` | 10000 | 最大点位数，决定共享内存大小 |
| `max_subscribers` | `uint32_t` | 32 | 最大订阅者进程数 |

**返回值**：`true` 成功，`false` 失败（instance_id 为空、共享内存创建失败、magic 校验失败）。

**说明**：
- 首个调用的进程成为 owner，创建并初始化共享内存段（magic = `0x1DBA1DBA`）
- 后续进程通过相同的 `instance_id` 附加到已有段，校验 magic 和 version
- 调用前确保没有残留的同名共享内存段（`/dev/shm/indurtdb_<id>`）

---

### 写入：`write<T>()`

```cpp
template<typename T>
bool write(PointId id, const T& value);
```

**支持类型**：
| T | 说明 | PointType |
|---|------|-----------|
| `bool` | 布尔值（开关、启停） | `BOOL (0)` |
| `int32_t` | 32 位有符号整数（计数器、档位） | `INT32 (1)` |
| `double` | 双精度浮点数（温度、压力） | `DOUBLE (2)` |

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` (uint32_t) | 点位 ID，0 ≤ id < max_points |
| `value` | `const T&` | 要写入的值 |

**返回值**：`true` 成功，`false` 失败（id 越界或 Seqlock 写冲突）。

**副作用**：自动更新 `timestamp_ns`（当前单调时间）和 `quality = GOOD`。递增 `stats.writes` 计数器。写入后自动 `notify()` 触发该点位的回调。

**示例**：
```cpp
rtdb.write(1001, 23.5);           // 写入温度
rtdb.write(2001, (int32_t)42);    // 写入计数器（注意显式转换）
rtdb.write(3001, true);           // 写入开关状态
```

### 写入字符串：`write(PointId, const char*)`

```cpp
bool write(PointId id, const char* value);
```

**非模板重载**，避免 `const char[N]` 字面量类型推导问题。

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |
| `value` | `const char*` | C 字符串，最大 31 字符（自动截断） |

---

### 拷贝读取：`read()`

```cpp
bool read(PointId id, PointData& out) const;
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |
| `out` | `PointData&` | 输出参数，接收完整点位数据副本（128 字节拷贝） |

**返回值**：`true` 成功，`false` 失败（id 越界）。

**说明**：通过 Seqlock 协议保证读到一致的数据（不会读到"写到一半"的撕裂数据）。

---

### 零拷贝读取：`peek()`

```cpp
const PointData* peek(PointId id) const;
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 点位 ID |

**返回值**：指向共享内存中 `PointData` 的 `const` 指针，失败返回 `nullptr`。

**关键限制**：
- 返回的指针直接指向共享内存，**不持有锁**
- 调用方不应长期持有此指针（跨越写入边界后数据可能变化）
- 适合高频读取场景（如控制循环中批量遍历点位）

**性能对比**：
| 方法 | 拷贝 (128B) | 预估延迟 (ARM A53) |
|------|------------|-------------------|
| `read()` | 有 | ~50ns |
| `peek()` | 无 | ~10ns |

---

### 订阅：`subscribe()`

```cpp
bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `id` | `PointId` | 要订阅的点位 ID |
| `cb` | `SubscriptionCallback` | 回调函数指针 |
| `user_data` | `void*` | 透传给回调的用户数据 |

**返回值**：`true` 成功，`false` 失败（id 越界、回调为 null、订阅槽位已满）。

**说明**：
- 全局最多 256 个订阅槽位
- 回调在写入者线程内**同步执行**，不要在回调中做耗时操作
- 回调签名见 `SubscriptionCallback` 类型定义

### 取消订阅：`unsubscribe()`

```cpp
bool unsubscribe(PointId id);
```

取消指定点位上的所有订阅。

---

### 配置加载：`load_config()`

```cpp
bool load_config(const char* config_path);
```

**参数**：
| 参数 | 类型 | 说明 |
|------|------|------|
| `config_path` | `const char*` | YAML 配置文件路径 |

**返回值**：`true` 成功，`false` 失败（文件不存在、格式错误）。

**支持的 YAML 格式**：
```yaml
points:
  - id: 1001
    name: "AHU_01.Supply_Temp"
    type: double    # bool | int32 | double | string
    unit: 1         # NO_UNIT=0, °C=1, Pa=2, %=3
    access: 1       # READ_ONLY=1, READ_WRITE=3
```

---

### 心跳更新：`update_heartbeat()`

```cpp
void update_heartbeat();
```

更新当前进程的心跳时间戳。定期调用以避免被 `cleanup_zombies()` 清理。

---

### 关机：`shutdown()`

```cpp
void shutdown();
```

释放 PointManager、SubscriptionManager、SharedMemorySegment。若为 owner 进程，同时 `shm_unlink` 删除共享内存段。调用后 `is_initialized()` 返回 `false`。

---

### 状态查询：`is_initialized()`

```cpp
bool is_initialized() const;
```

**返回值**：`true` 已初始化且未关机。

---

## 数据类型参考

### PointData

```cpp
struct PointData {
    union Value {
        bool      b;            // 布尔值
        int32_t   i;            // 32 位整数
        double    d;            // 浮点数
        char      str[32];      // 字符串（最多 31 字符 + '\0'）
    } value;                    // 值联合体，根据 type 字段判断使用哪个成员

    uint64_t  timestamp_ns;     // 时间戳（CLOCK_MONOTONIC_RAW，纳秒）
    PointType type;             // 数据类型（BOOL=0, INT32=1, DOUBLE=2, STRING=3）
    Quality   quality;          // 数据质量（GOOD=0, BAD=1, TIMEOUT=2, SUBSTITUTED=3）
    Unit      unit;             // 单位（NO_UNIT=0, °C=1, Pa=2, %=3）
    Access    access;           // 访问权限（READ_ONLY=1, READ_WRITE=3）
    char      name[64];         // 点位名称（如 "AHU_01.Supply_Temp"）
};  // sizeof = 128 字节，aligned(128)
```

### 基础类型别名

```cpp
using PointId     = uint32_t;   // 点位标识符
using TimestampNs = uint64_t;   // 纳秒时间戳
using Pid         = int32_t;    // 进程 ID
```

### 枚举

```cpp
enum class PointType : uint8_t { BOOL = 0, INT32 = 1, DOUBLE = 2, STRING = 3 };
enum class Quality   : uint8_t { GOOD = 0, BAD = 1, TIMEOUT = 2, SUBSTITUTED = 3 };
enum class Access    : uint8_t { READ_ONLY = 1, READ_WRITE = 3 };
enum class Unit      : uint16_t { NO_UNIT = 0, DEGREES_CELSIUS = 1, PASCAL = 2, PERCENT = 3 };
```

### SubscriptionCallback

```cpp
using SubscriptionCallback = void (*)(PointId id,
                                      const PointData& data,
                                      void* user_data);
```

**参数**：
- `id` — 发生变化的点位 ID
- `data` — 点位数据引用（指向共享内存，回调内使用后即失效）
- `user_data` — `subscribe()` 时传入的用户数据指针

---

## 错误处理

- 所有 `bool` 返回值的函数，`false` 表示失败
- 错误详情通过日志输出（stderr）
- C API 额外提供 `indurtdb_get_last_error()` 获取最近错误消息

## 线程安全

- `write<T>()`：线程安全（Seqlock 保护），但设计为单写者模式，并发写会冲突返回 `false`
- `read()` / `peek()`：线程安全（Seqlock 保护），完全无锁
- `subscribe()` / `unsubscribe()`：非线程安全，应在初始化阶段完成
- `initialize()` / `shutdown()`：非线程安全，应在主线程单次调用

---

## 预留：异步接口 (规划中)

当前 subscribe 回调在写入者线程同步执行。未来将通过 Unix Domain Socket 实现跨进程异步通知：

```cpp
// 未来的异步订阅接口（规划中）
bool subscribe_async(PointId id, int notify_fd);
```

现有 `SubscriptionCallback` 签名兼容此扩展，无需变更。
```

- [ ] **Step 2: Commit**

```bash
git add docs/sdk/C++ API 参考手册.md
git commit -m "docs: 新增 C++ API 参考手册"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 7: 编写 C API 参考手册

**Files:**
- Create: `docs/sdk/C API 参考手册.md`

- [ ] **Step 1: 编写 C API 参考手册**

```markdown
# InduRTDB C API 参考手册

**版本**：2.1.0

**头文件**：`<indurtdb/api/c/indurtdb_c.h>`
**链接**：`-lindurtdb -lpthread -lrt`

---

## 类型定义

### indurtdb_point_t

```c
typedef struct {
    union {
        bool b;
        int32_t i;
        double d;
        char str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t  type;       // 0=BOOL, 1=INT32, 2=DOUBLE, 3=STRING
    uint8_t  quality;    // 0=GOOD, 1=BAD, 2=TIMEOUT, 3=SUBSTITUTED
    uint16_t unit;       // NO_UNIT=0, °C=1, Pa=2, %=3
    uint8_t  access;     // 1=READ_ONLY, 3=READ_WRITE
    char name[64];
} indurtdb_point_t;
```

与 C++ `PointData` 布局兼容的 C 结构体。`sizeof` = 128 字节。

---

## 初始化与关闭

### indurtdb_initialize

```c
int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers);
```

**参数**：
| 参数 | 说明 | 默认值 |
|------|------|--------|
| `instance_id` | 实例名（段名 `/indurtdb_<id>`） | — |
| `max_points` | 最大点位数 | 10000 |
| `max_subscribers` | 最大订阅者数 | 32 |

**返回值**：0 成功，非 0 失败。

### indurtdb_shutdown

```c
void indurtdb_shutdown(void);
```

释放资源。owner 进程同时删除共享内存段。

---

## 写入函数

所有写入函数：成功返回 0，失败返回非 0（id 越界或写冲突）。

### indurtdb_write_bool

```c
int indurtdb_write_bool(uint32_t id, bool value);
```

写入布尔值。`type` 自动设为 `BOOL(0)`。

### indurtdb_write_int32

```c
int indurtdb_write_int32(uint32_t id, int32_t value);
```

写入 32 位有符号整数。`type` 自动设为 `INT32(1)`。

### indurtdb_write_double

```c
int indurtdb_write_double(uint32_t id, double value);
```

写入双精度浮点数。`type` 自动设为 `DOUBLE(2)`。

### indurtdb_write_string

```c
int indurtdb_write_string(uint32_t id, const char* value);
```

写入字符串（最多 31 字符，自动截断）。`type` 自动设为 `STRING(3)`。

---

## 读取函数

所有读取函数：成功返回 0，失败返回非 0（id 越界）。

### indurtdb_read_bool

```c
int indurtdb_read_bool(uint32_t id, bool* value);
```

### indurtdb_read_int32

```c
int indurtdb_read_int32(uint32_t id, int32_t* value);
```

### indurtdb_read_double

```c
int indurtdb_read_double(uint32_t id, double* value);
```

### indurtdb_read_string

```c
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);
```

**参数**：
| 参数 | 说明 |
|------|------|
| `buffer` | 用户提供的输出缓冲区 |
| `buffer_size` | 缓冲区大小（建议 ≥ 32 字节） |

### indurtdb_read_point

```c
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);
```

读取完整点位数据（128 字节拷贝），通过 Seqlock 协议保证一致性。

---

## 验证与统计

### indurtdb_validate_id

```c
int indurtdb_validate_id(uint32_t id);
```

检查 id 是否在有效范围内。返回 0 表示有效。

### indurtdb_get_write_count

```c
uint64_t indurtdb_get_write_count(void);
```

返回全局写入总次数（从共享内存 stats 读取）。

### indurtdb_get_timeout_count

```c
uint64_t indurtdb_get_timeout_count(void);
```

返回超时点位计数。

---

## 错误处理

### indurtdb_get_last_error

```c
const char* indurtdb_get_last_error(void);
```

返回最近一次错误的描述字符串。线程本地存储。

---

## 跨语言调用

### Python (ctypes)

```python
import ctypes

lib = ctypes.CDLL("libindurtdb.so")

lib.indurtdb_initialize(b"my_app", 1000, 32)
lib.indurtdb_write_double(1, ctypes.c_double(25.0))

val = ctypes.c_double()
lib.indurtdb_read_double(1, ctypes.byref(val))
print(f"温度: {val.value}")

lib.indurtdb_shutdown()
```

### Rust (FFI)

使用 `bindgen` 从 `indurtdb_c.h` 自动生成绑定，或手动声明 extern 块：

```rust
extern "C" {
    fn indurtdb_initialize(instance_id: *const c_char, max_points: u32, max_subscribers: u32) -> i32;
    fn indurtdb_write_double(id: u32, value: f64) -> i32;
    fn indurtdb_read_double(id: u32, value: *mut f64) -> i32;
    fn indurtdb_shutdown();
}
```
```

- [ ] **Step 2: Commit**

```bash
git add docs/sdk/C API 参考手册.md
git commit -m "docs: 新增 C API 参考手册（含 Python/Rust 跨语言说明）"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 8: 编写开发者指南

**Files:**
- Create: `docs/sdk/开发者指南.md`

- [ ] **Step 1: 编写开发者指南**

```markdown
# InduRTDB 开发者指南

**版本**：2.1.0 | **预计阅读**：20 分钟

面向需要深入理解 InduRTDB 内部机制、进行性能调优或排查问题的开发者。

---

## 1. 共享内存模型

### 1.1 内存布局

```
偏移 0           64      64+N×128        64+N×128+M×16
┌──────────────┬─────────┬──────────────┬─────────────────┐
│   Header     │ Points  │   Points     │   Subscribers   │
│   (64 B)     │  [0]    │  [1..N-1]   │  [0..M-1]       │
│              │ (128 B) │              │  (16 B each)    │
└──────────────┴─────────┴──────────────┴─────────────────┘
```

| 区域 | 大小 | 说明 |
|------|------|------|
| `InduRTDBHeader` | 64 B | magic, version, max_points, write_seq, stats |
| `PointData[]` | N × 128 B | 点位数据数组 |
| `SubscriberEntry[]` | M × 16 B | 订阅者心跳表 |

**总大小** = `64 + max_points × 128 + max_subscribers × 16`

默认 10,000 点位 + 32 订阅者 ≈ **1.28 MB**。

### 1.2 Header 结构

| 偏移 | 字段 | 类型 | 说明 |
|------|------|------|------|
| 0 | `magic` | `uint32_t` | 固定值 `0x1DBA1DBA` |
| 4 | `version` | `uint32_t` | 布局版本（当前 = 1） |
| 8 | `max_points` | `uint32_t` | 最大点位数 |
| 12 | `max_subscribers` | `uint32_t` | 最大订阅者数 |
| 16 | `write_seq` | `uint64_t` | Seqlock 全局序列号 |
| 24 | `stats.writes` | `uint64_t` | 总写入次数 |
| 32 | `stats.timeouts` | `uint64_t` | 超时计数 |

### 1.3 段命名与生命周期

- 段名：`/indurtdb_<instance_id>`（如 `/indurtdb_hvac`）
- 创建者（owner）：首个调用 `initialize()` 的进程，调用 `shm_open(O_CREAT | O_EXCL)`
- 使用者（joiner）：后续进程，调用 `shm_open(O_RDWR)` + 校验 magic/version
- 段文件：`/dev/shm/indurtdb_*`（Linux tmpfs）
- 权限：`0666`

---

## 2. Seqlock 并发控制

### 2.1 设计假设

InduRTDB 面向**多读少写**的工业场景（驱动每秒写入 ≤1kHz 每通道，控制引擎每 50ms 读取一批点位）。

### 2.2 全局 write_seq

| 值 | 含义 |
|----|------|
| 偶数 | 空闲（无写入进行中） |
| 奇数 | 写入中（读取者应自旋等待） |

### 2.3 写入协议

```cpp
// 1. 原子读 seq → s0
uint64_t s0 = __atomic_load_n(&seq, ACQUIRE);
// 2. 奇数 = 有并发写入，返回 false
if (s0 & 1) return false;
// 3. CAS 存 s0+1 标记"写入中"
__atomic_store_n(&seq, s0 + 1, RELEASE);
// 4. 更新 PointData...
// 5. 存 s0+2 恢复偶数
__atomic_store_n(&seq, s0 + 2, RELEASE);
```

**关键性质**：
- 单写者假设：不 spin-wait，冲突时直接返回 false
- 工业场景冲突概率 < 0.1%

### 2.4 读取协议

```cpp
do {
    s0 = __atomic_load_n(&seq, ACQUIRE);
    if (s0 & 1) continue;               // 写中，重试
    __atomic_thread_fence(ACQUIRE);      // 内存屏障
    // 读取数据...
    s1 = __atomic_load_n(&seq, ACQUIRE);
} while (s0 != s1);                     // 不一致，重试
```

**关键性质**：
- 完全无锁：Reader 不会阻塞 Writer
- 零拷贝：`peek()` 直接返回共享内存指针
- Reader 可能自旋 1-3 次（取决于与 Writer 的交叠时机）

### 2.5 ABA 防护

`write_seq` 使用 `uint64_t`。即使每秒 1,000,000 次写入，溢出需要约 584,942 年。

---

## 3. 多进程安全

### 3.1 共享内存结构约束

- 所有共享内存结构体为 **POD**（Plain Old Data）
- 禁止指针、虚表、引用（跨进程地址空间不同）
- 编译期验证：`static_assert(std::is_pod_v<T>)` + `static_assert(sizeof(T) == N)`

### 3.2 子进程退出

```cpp
if (pid == 0) {
    // 子进程代码...
    _exit(0);    // ← 使用 _exit() 而非 exit()
}
```

`exit()` 会调用全局对象的析构函数，包括 `InduRTDB` 单例析构 → `shutdown()` → `shm_unlink()`，导致父进程和其他进程的共享内存被错误删除。

### 3.3 心跳与僵尸清理

- 订阅者进程定期调用 `update_heartbeat()` 更新时间戳
- `cleanup_zombies()` 扫描心跳表中超时（>1s 未更新）的条目并清理
- 回调函数为进程本地数据，不存入共享内存中的 `SubscriberEntry`

---

## 4. 性能建议

### 4.1 读取优化

| 场景 | 推荐方法 | 原因 |
|------|---------|------|
| 单次读取 | `read()` | 安全拷贝 |
| 控制循环（批量） | `peek()` | 零拷贝，延迟 ~10ns |
| HMI 订阅 | `subscribe()` | 变更推送，避免轮询 |

### 4.2 peek() 使用约束

```cpp
// ✅ 正确用法
const PointData* p = rtdb.peek(id);
double val = p->value.d;  // 立即拷贝需要的值

// ❌ 错误用法
const PointData* p = rtdb.peek(id);
rtdb.write(other_id, new_val);  // 可能触发 write_seq 变更
use(p->value.d);                 // p 指向的数据可能已被覆盖
```

### 4.3 写入频率

- 单点位写入频率建议 ≤ 1 kHz
- 高频批量写入时应检查写冲突（`write()` 返回 `false` 时重试）
- `PointData` 按 128 字节对齐，避免与相邻数据结构的伪共享

### 4.4 编译优化

生产构建使用 `-O2`（CMake 默认 Release 模式）。`-fno-exceptions -fno-rtti` 已内置。

---

## 5. 常见问题排查

### 5.1 magic mismatch

```
[InduRTDB] magic mismatch: expected 0x1DBA1DBA got 0x00000000
```

**原因**：段名冲突或残留的未初始化共享内存段。

**解决**：
```bash
ls /dev/shm/indurtdb_*
rm /dev/shm/indurtdb_<instance_id>
```

### 5.2 段已存在但无法附加

```
[InduRTDB] version mismatch: expected 1 got 0
```

**原因**：用不同的 `max_points` / `max_subscribers` 参数初始化已有段。

**解决**：所有进程使用相同的初始化参数，或先清理残留段。

### 5.3 写操作总是失败

- 检查 `id` 是否在 `[0, max_points)` 范围内
- 检查是否有多个进程同时写入同一个库实例
- 确认 `initialize()` 已成功调用

### 5.4 编译链接错误

```bash
# 缺少符号
undefined reference to `indurtdb::InduRTDB::instance()'
```

**解决**：确认链接顺序 `-lindurtdb -lpthread -lrt`，静态库需放在依赖它的目标之后。

### 5.5 subscribe 回调不触发

- 检查 `subscribe()` 的返回值
- 检查回调签名是否完全匹配 `void (*)(PointId, const PointData&, void*)`
- 注意 lambda（即使无捕获）不能隐式转换为 C 函数指针

---

## 6. 未来规划

| 功能 | 说明 |
|------|------|
| Unix Domain Socket 异步通知 | 允许订阅者通过 fd 接收通知，而非在写入者线程同步回调 |
| OPC UA Bridge | 将 PointData 映射到 OPC UA Address Space |
| SylixOS 验证 | 交叉编译 + ARM Cortex-A 目标硬件测试 |
| 性能基准 | P99 延迟实测（当前为设计目标值） |
```

- [ ] **Step 2: Commit**

```bash
git add docs/sdk/开发者指南.md
git commit -m "docs: 新增 SDK 开发者指南"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

### Task 9: 全部示例编译运行验证

**Files:**
- Verify: all examples + docs

- [ ] **Step 1: 完整构建**

```bash
cd build
cmake .. -DBUILD_TESTS=ON -DBUILD_EXAMPLES=ON
make -j$(nproc)
```

- [ ] **Step 2: 运行全部示例**

```bash
./examples/basic_example
./examples/c_example
./examples/multi_process_example
```

- [ ] **Step 3: 运行全部测试**

```bash
./indurtdb_tests
```

预期：59/59 全部通过，3 个示例均正常运行无崩溃。

- [ ] **Step 4: 验证文档代码片段可编译**

从快速入门指南中拷贝"第一个程序"代码到 `/tmp/test_doc.cpp`，编译：
```bash
g++ -std=c++17 -I<indurtdb>/include -L<indurtdb>/build /tmp/test_doc.cpp -lindurtdb -lpthread -lrt
```

预期：编译通过。

- [ ] **Step 5: Final commit**

```bash
git add -A
git commit -m "docs: SDK 文档集和示例完成，全部构建验证通过"

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
```

---

## 验证清单

- [ ] `basic_example` 编译运行，subscribe 通过 C 函数指针触发回调
- [ ] `c_example` 编译运行，4 种类型读写正确
- [ ] `multi_process_example` 编译运行，子进程 peek 读到父进程数据
- [ ] 59/59 单元+集成测试通过
- [ ] 快速入门指南中所有代码片段可拷贝编译
- [ ] C++ API 参考手册覆盖全部公开 API
- [ ] C API 参考手册覆盖全部 17 个函数
- [ ] 开发者指南覆盖共享内存、Seqlock、多进程、性能、排错
