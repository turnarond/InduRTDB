# InduRTDB × node-server 集成设计文档（v3.2.0）

**版本：1.0**
**日期：2026-08-15**
**状态：已批准（待实现）**
**依据：`06-开发规划/05-技术演进规划.md` 中期目标（G7，4.1–4.5）**

---

## 1. 背景与目标

InduRTDB v3.1.0 已完成核心库与产品化（find_package / pkg-config / FetchContent、CI、崩溃自愈、x86 实测）。下一版本（v3.2.0）的目标是让 **node-server 以 InduRTDB 作为其存储后端**，替换 node-server 内部的"进程内 C++ RTDB"，把存储层升级为跨进程、类型化、带工业语义、崩溃自愈的高性能实时层。

**目标**：node-server 通过既有 `StorageInterface` 抽象无缝接入 InduRTDB 后端，实现"node-server 用 InduRTDB"的端到端可用集成。

## 2. 边界与职责划分

两者分层互补，非替代：

```
┌─────────────────────────────────────────────────┐
│  node-server（数据中枢上层，保留）                 │
│  · OPC UA Server（北向 → SCADA/HMI）             │
│  · SQLite 持久化 / 历史                          │
│  · Compute Engine / IPC / 业务逻辑              │
│  · StorageInterface 抽象 ← InduRTDB 在此接入     │
├─────────────────────────────────────────────────┤
│  InduRTDB（存储/实时数据层，本次提供）             │
│  · 跨进程共享内存点位存储（shm + seqlock）         │
│  · 按 ID 读写 / 批量 / 订阅 / 崩溃自愈 / 超时      │
│  · 工业语义 + name→ID 映射（v3.2.0 新增）        │
└─────────────────────────────────────────────────┘
```

- InduRTDB **只**替换 node-server 的存储层；北向/持久化/计算/IPC/业务仍归 node-server。
- InduRTDB **不覆盖**（也不应覆盖）OPC UA、SQLite、Compute Engine 等——这些是 4.3/4.4 的"复用验证"对象，非新建。

## 3. 方案选型（方案 A：InduRTDB 薄 API + 适配器侧元数据）

**决策**：InduRTDB 只新增最小查询 API（name→ID + 元数据），**共享内存布局零改动**（保持 v2.x 逐字节兼容）；字段缺口（driver/device/version）由 node-server 适配器进程内侧表承担。

**否决方案 B**（扩展 InduRTDB 点位结构塞入 driver/device/version）：破坏 v2.x 逐字节兼容、InduRTDB 膨胀、违背精简存储层定位。

## 4. InduRTDB v3.2.0 新增 API（最小集）

所在仓库：`src/comms/InduRTDB`。原则：只加查询，不改存储/布局/既有 API。

```c
/* name→ID 查询；未找到返回 -1，置 get_last_error */
int indurtdb_get_id_by_name(const char* name);

/* 按 name 取配置元数据（供值类型转换）；name 未找到返回 -1 */
int indurtdb_get_meta_by_name(const char* name,
                              uint8_t* type, uint16_t* unit, uint8_t* access);
```

- name↔ID 关系来自 `indurtdb_load_config()` 加载的点位配置（YAML，含 id/name/type/unit/access）。
- 需在 InduRTDB 内维护一张 name→(id,type,unit,access) 的只读查找表：**load_config 时构建于进程内（进程本地内存，不进共享内存）**，以保持 shm 布局兼容；各进程 load_config 后各自持有一份一致副本。
- 新增单元测试覆盖这两个 API（命中/未命中/未初始化/NULL 入参）。

## 5. node-server 侧改动

所在仓库：`src/service/node-server`。

### 5.1 InduRTDBAdapter（核心）
新增 `class InduRTDBAdapter : public StorageInterface`（对齐 `core/rtdb_adapter.hpp` 的 RTDBAdapter 模式），实现全部 `StorageInterface` 方法：

| 方法 | 实现 |
|---|---|
| `registerTag(name)` | `indurtdb_get_id_by_name(name)` → 记入适配器 name→ID 缓存 + 已注册集 |
| `unregisterTag(name)` | 从适配器已注册集移除（InduRTDB 点位定长，不真删） |
| `setTag(name, val_str, ts, driver, device)` | name→ID → val_str 按 type 转类型值 → `indurtdb_write_<type>(id, v)`；ts ms→ns；driver/device 存侧表 |
| `getTag(name, TagRecord&)` | ID → `indurtdb_read_point(id)` → 组装 TagRecord（值→string、ts ns→ms、driver/device 取侧表、version 取侧表计数） |
| `getTags(names)` / `setTags(entries)` | 循环调用 getTag/setTag |
| `getAllTags()` / `size()` | 遍历适配器已注册集，逐点读 InduRTDB 组装 |
| `healthCheck()` | `indurtdb_is_initialized()` + magic 校验 |
| `addUpdateCallback(cb)` / `removeUpdateCallback(id)` | 对 InduRTDB 按点订阅，C 回调 trampoline → 转 TagRecord → fan-in 到全局 UpdateCallback |

### 5.2 字段缺口处理（适配器侧表）
| 字段 | 处理 |
|---|---|
| `driver_name`/`device_name` | `std::unordered_map<id, {driver, device}>`，setTag 时写入 |
| 每点 `version` | `std::unordered_map<id, uint64_t>` 自增计数，setTag 时 +1 |
| getAllTags 激活集 | 适配器维护已注册 name 集合，无需 InduRTDB 提供遍历 |

### 5.3 StorageFactory / storage_config
- `StorageFactory::StorageType` 增加 `INDURTDB`；`createStorage` 增加对应分支返回 `InduRTDBAdapter`。
- `storage_config.hpp` 增加 INDURTDB 配置（instance_id、点位配置路径），`loadStorageConfig` 解析。

## 6. 数据流（经适配器）

```
写: setTag(name,val_str,ts,driver,device)
     → Adapter: name→ID(缓存) → val_str→类型值(按type) → indurtdb_write_<type>(id,v) → shm
     → driver/device/version 写入适配器侧表
读: getTag(name) → ID → indurtdb_read_point(id) → TagRecord(值→string, ts ns→ms, driver/device/version 取侧表)
订阅: Adapter 对 InduRTDB 按点订阅 → C trampoline → TagRecord → 全局 UpdateCallback(fan-in)
```

## 7. 错误处理

| 情形 | 处理 |
|---|---|
| name 未在配置 | get_id_by_name 返回 -1，置 `get_last_error`；setTag/getTag 失败 |
| InduRTDB 未初始化 | API 返回错误，适配器映射为 StorageInterface 失败 |
| 值类型转换失败 | setTag 失败并置错误 |
| 订阅数超 256 | subscribe 失败，适配器回错 |

## 8. 测试策略

- **InduRTDB**：新增 `test_c_name_lookup.cpp` 覆盖 get_id_by_name / get_meta_by_name（命中/未命中/未初始化/NULL）。
- **node-server**：InduRTDBAdapter 单元测试（真实 InduRTDB 后端，覆盖 StorageInterface 全方法 + 字段侧表）；StorageFactory 选型测试。
- **端到端（4.5）**：node-server 以 INDURTDB 后端运行，写→读→（北向/持久化复用验证）。

## 9. 分期与验收

**MVP（v3.2.0 首发）= 4.1 适配器 + 4.2 映射 + 基础端到端（4.5 核心）**
- 验收：node-server 以 InduRTDB 后端 setTag/getTag/subscribe 全通；字段缺口经侧表正确；InduRTDB 布局零改动、既有测试全绿。

**后续 = 4.3 北向 OPC UA + 4.4 持久化 SQLite（复用验证）**
- 验收：InduRTDB 后端下，既有 OPC UA Server 能暴露数据、SQLite 能归档历史，无回归。

## 10. 跨仓库说明

- 本设计跨两个仓库：InduRTDB（`src/comms/InduRTDB`，独立 git）与 node-server（`src/service/node-server`，edge-framework 主仓）。
- InduRTDB 侧改动（第 4 节）随 v3.2.0 在本仓库交付；node-server 侧改动（第 5 节）需在 edge-framework 主仓另开分支落地。
- 两侧通过"同一份点位配置（YAML name↔ID）"对齐。
- **实现拆分**：因跨仓库，实现计划按序拆为两段——先 InduRTDB 侧（name→ID API + 测试，本仓库），后 node-server 侧（适配器 + 工厂 + 配置 + 端到端，edge-framework 主仓）。前者是后者的依赖。
