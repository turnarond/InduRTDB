# InduRTDB SDK 用户文档与示例 设计规格说明

**版本**：1.0
**日期**：2026-05-17
**状态**：已批准

---

## 1. 背景与动机

InduRTDB 当前 API 实现已完备（C++ PIMPL 单例 + C ABI 17 函数），构建产物为 `libindurtdb.a` 静态库。但缺少系统化的用户文档，仅有一个 `basic_example.cpp` 示例且存在 bug（`subscribe` 使用了 lambda 而非 C 函数指针 `SubscriptionCallback`）。用户无法通过阅读文档独立完成集成。

目标：交付一套标准的 SDK 文档集 + 修正/新增的示例代码，让用户能在 5 分钟内完成首次读写，并能按需查阅完整 API 参考。

---

## 2. 交付物

### 2.1 文档（4 份）

| 文档 | 路径 | 目标 |
|------|------|------|
| 快速入门指南 | `docs/sdk/快速入门指南.md` | 5 分钟首次读写 |
| C++ API 参考手册 | `docs/sdk/C++ API 参考手册.md` | 每个函数/类型的完整说明 |
| C API 参考手册 | `docs/sdk/C API 参考手册.md` | 17 个 C 函数 + 跨语言说明 |
| 开发者指南 | `docs/sdk/开发者指南.md` | 架构概念、多进程安全、性能、排错 |

### 2.2 示例代码（3 个）

| 文件 | 路径 | 状态 | 内容 |
|------|------|------|------|
| C++ 基础示例 | `examples/basic_example.cpp` | **修复** | 修正 subscribe lambda→C 函数指针 |
| C 示例 | `examples/c_example.c` | **新增** | 纯 C 读写 bool/int/double/string |
| 多进程示例 | `examples/multi_process_example.cpp` | **新增** | fork 父子进程共享读写 |

### 2.3 现有资产（不变）

- `libindurtdb.a` 静态库
- `<indurtdb.hpp>` 公共头文件
- `<indurtdb/api/c/indurtdb_c.h>` C 头文件

---

## 3. 各文档内容大纲

### 3.1 快速入门指南

```
1. 前置要求（GCC ≥7.5, CMake ≥3.15, pthread, rt）
2. 构建 SDK（cmake + make，输出 libindurtdb.a）
3. 链接到项目（CMake target_link_libraries / 命令行 -lindurtdb -lpthread -lrt）
4. 第一个程序（完整可编译运行的 C++ 代码）
5. 写入数据（bool / int32_t / double / string 四种类型）
6. 读取数据（read() 拷贝 vs peek() 零拷贝）
7. 订阅数据变更（C 函数指针回调）
8. 加载 YAML 配置
9. 多进程场景（首个进程创建共享内存，后续进程自动加入验证 magic/version）
10. 关机与清理
```

### 3.2 C++ API 参考手册

```
1. InduRTDB 类
   - instance()           — 单例获取
   - initialize()         — 初始化（创建或附加共享内存）
   - write<T>()           — 模板写入（bool/int32_t/double）
   - write(const char*)   — 字符串写入（非模板重载）
   - read()               — 拷贝读取（返回 PointData 副本）
   - peek()               — 零拷贝读取（返回共享内存指针，附使用注意事项）
   - subscribe()          — 注册回调
   - unsubscribe()        — 注销回调
   - load_config()        — YAML 配置加载
   - update_heartbeat()   — 心跳更新
   - shutdown()           — 关机
   - is_initialized()     — 状态查询

2. 数据类型
   - PointData / PointData::Value 联合体
   - PointId, TimestampNs, Pid
   - Quality / Access / Unit 枚举
   - SubscriptionCallback 函数指针类型（void (*)(PointId, const PointData&, void*)）

3. 返回值约定
   - 全部使用 bool 返回，无异常
   - 失败时通过 get_last_error()（C API）或日志输出错误信息

4. 预留：异步接口扩展
   - 未来 subscribe 异步通知通过 Unix Domain Socket 实现
   - SubscriptionCallback 签名兼容异步扩展，无需变更
```

### 3.3 C API 参考手册

```
1. 类型定义
   - indurtdb_point_t（与 PointData 布局兼容的 C 结构体）

2. 初始化与关闭
   - indurtdb_initialize(instance_id, max_points, max_subscribers)
   - indurtdb_shutdown()

3. 写入系列
   - indurtdb_write_bool(id, value)
   - indurtdb_write_int32(id, value)
   - indurtdb_write_double(id, value)
   - indurtdb_write_string(id, value)

4. 读取系列
   - indurtdb_read_bool(id, *value)
   - indurtdb_read_int32(id, *value)
   - indurtdb_read_double(id, *value)
   - indurtdb_read_string(id, *buffer, buffer_size)
   - indurtdb_read_point(id, *point_data)

5. 统计与错误
   - indurtdb_get_write_count()
   - indurtdb_get_timeout_count()
   - indurtdb_get_last_error()

6. 跨语言调用
   - Python ctypes 加载 libindurtdb.so 简要示例
   - Rust FFI bindgen 方向说明
```

### 3.4 开发者指南

```
1. 共享内存模型
   - 内存布局：[Header 64B][PointData[max] 128B each][SubscriberEntry[max] 16B each]
   - 段命名：/indurtdb_<instance_id>
   - 创建者（owner）与使用者（joiner）的职责区别
   - magic 校验 (0x1DBA1DBA) 与 version 校验

2. Seqlock 并发控制
   - 全局 write_seq：偶数=空闲，奇数=写入中
   - 单写者假设：写冲突时返回 false，调用方可重试
   - 读路径完全无锁：seqlock_read 自旋直到序列号一致且为偶数

3. 多进程安全
   - 子进程必须使用 _exit() 而非 exit()（避免继承析构调用 shm_unlink）
   - 心跳机制：定期 update_heartbeat() + cleanup_zombies() 清理僵尸进程
   - 回调函数为进程本地数据，不存入共享内存

4. 性能建议
   - peek() 零拷贝适用于高频读取场景
   - 调用方不应长期持有 peek() 返回的指针（跨越写入边界）
   - PointData 按 128 字节对齐，避免伪共享

5. 常见问题排查
   - magic mismatch —— 共享内存段名冲突或布局不兼容
   - permission denied —— shm_open 权限 (0666)
   - 写入失败 —— id 越界或写冲突（多写者并发）
   - 编译链接错误 —— 缺少 -lpthread -lrt

6. 未来接口规划
   - Unix Domain Socket 异步跨进程通知
   - OPC UA Bridge 桥接模块
```

---

## 4. 示例代码设计

### 4.1 basic_example.cpp（修复）

**问题**：当前版本使用 `rtdb.subscribe(2001, [](const indurtdb::PointData& p) {...})` 传入 lambda，但实际 API 签名要求 `SubscriptionCallback`（C 函数指针），lambda 无法隐式转换。

**修复**：改为先定义静态回调函数，再调用 `subscribe()`。

```cpp
// 修复后
static void on_pump_state_change(PointId id, const PointData& data, void* user_data) {
    (void)id;
    (void)user_data;
    printf("  水泵状态变化: %s\n", data.value.b ? "启动" : "停止");
}

// 使用
rtdb.subscribe(2001, on_pump_state_change, nullptr);
```

### 4.2 c_example.c（新增）

完整 C 语言示例，覆盖：
- 初始化 → 写 bool/int32_t/double/string → 读取 → 统计 → 关闭

### 4.3 multi_process_example.cpp（新增）

演示多进程场景：
- 父进程初始化 → 写入数据
- fork 子进程 → 读取父进程写入的数据（peek 零拷贝）
- 子进程 _exit(0) 安全退出

---

## 5. 构建集成

更新 `examples/CMakeLists.txt`，新增两个示例目标的编译规则。

---

## 6. 不包含的内容

- **不修改 `libindurtdb.a` 的核心代码**（仅修复示例 bug）
- **不添加异步通知实现**（文档中预留接口说明即可）
- **不新增 API 函数**（纯文档 + 示例层面）

---

## 7. 验证标准

- `basic_example` 编译运行，零崩溃，subscribe 正确触发回调
- `c_example` 编译运行，读写四种类型均正确
- `multi_process_example` 编译运行，子进程读到父进程数据
- 四份文档中所有代码片段可直接拷贝编译通过
