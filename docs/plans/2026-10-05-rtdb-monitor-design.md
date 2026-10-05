# InduRTDB 监控软件（rtdb-monitor）设计文档

- **日期**：2026-10-05
- **状态**：设计已确认（待实施）
- **关联**：v3.4 T9/T10 的 rtdbd 管控通道；复用 `rtdbd/protocol.h` UDS 协议
- **目标**：提供类 Redis Insight 的 RTDB Web 监控台——枚举全部点位、实时值推送、按名/id 查看、设置点位值。

---

## 1. 背景与动机

InduRTDB 当前对外暴露能力的通道有两条：

1. **核心库 C API**（进程内，共享内存直读直写）——不适合做跨进程运维界面。
2. **rtdbd 守护进程 UDS 二进制协议**（`PING/WRITE/SUBSCRIBE→NOTIFY/FIND_BY_NAME/GET_META/SET_META`）——进程级管控通道，受 `SO_PEERCRED` + uid 策略鉴权。

现有协议**缺少"按 id 读当前值"和"枚举全部点位"的 opcode**，无法支撑"监控所有点位"的客户端。
而 edge-framework 的 node-server 已用 VSOA 向 HMI 推送点位实时值（`HmiPointValueDto { pointId, value, quality, ts }`）。
本设计在**不污染 rtdbd 内核、不引入 C++ 依赖**的前提下，补足监控所需的只读能力，并让浏览器侧报文与 node-server 的 HMI 契约**schema 对齐**，实现"统一接口"。

---

## 2. 总体架构

```
浏览器 (原生 JS 单页, 无构建)  ⇄  WebSocket / HTTP  ⇄  Python Web 服务 (FastAPI)
                                                          │ UDS 二进制协议 (纯 Python 复刻 protocol.h)
                                                          ▼
                                                       rtdbd 守护进程  ⇄  共享内存 (RTDB)
```

- 新增目录 `tools/rtdb-monitor/`（与 `tools/irt_diag` 同级的独立 Python 工程）。
- **零 C 依赖**：Web 服务用 Python `socket` + `struct` 复刻 `rtdbd/include/rtdbd/protocol.h` 的报文，
  不引入 protobuf / VSOA / 任何第三方 RPC 栈。
- 前端用原生 HTML/JS，由 FastAPI 以 `StaticFiles` 直接托管，**无需 Node 构建链**；
  部署只需 `pip install fastapi uvicorn`（可选 `pydantic`）。
- **分层解耦**：
  - 监控后端 ↔ rtdbd 走 UDS 二进制协议（内部通道，与 node-server 无关）。
  - 浏览器 ↔ 监控后端走 WebSocket/HTTP，JSON 报文在 schema 层对齐 node-server HMI DTO。

---

## 3. rtdbd 协议扩展（C 改动）

在 `rtdbd/include/rtdbd/protocol.h` 新增两个 **只读、免鉴权** opcode（与 `FIND_BY_NAME`/`GET_META` 同权）：
鉴权策略依据 v3.4 设计——**读免鉴权、写/设元数据走 `SO_PEERCRED`**。

| opcode | 值 | 请求负载 | 响应负载 | 鉴权 |
|---|---|---|---|---|
| `RTDBD_OP_GET` | 10 | `point_id`(4B) | `{point_id(4), type(1), quality(1), reserved(2), value_bits(8), timestamp_ns(8), source_ts_ns(8)}` ≈ 32B | 免鉴权 |
| `RTDBD_OP_LIST` | 11 | `max`(4B, 0=全部) + `offset`(4B) | 点位信息数组 `{point_id(4), type(1), access(1), reserved(2), name[64]}`；分片流式返回 | 免鉴权 |

设计要点：
- 这两个 opcode 是**加法式**扩展，旧客户端忽略未知 opcode，**协议版本号不 bump**（保持 `RTDBD_PROTO_VERSION 2`）。
- `GET` 从共享内存无锁读（复用 `irt_pm_read` 的 seqlock 重试范式），**不进入写热路径**。
- `LIST` 遍历索引/点位表返回 `{id, name, type, access}`；大数据集按 `max/offset` 分片，
  单包不超过合理上限（如 4KB），避免一次拷贝过多。
- 写值仍走既有 `RTDBD_OP_WRITE`；元数据写走 `RTDBD_OP_SET_META`——二者继续受 `SO_PEERCRED` + uid 策略管控。
- 须在 `rtdbd.c` 的 `handle_request` 增加分派，并为新增负载补 `IRT_STATIC_ASSERT` 锁死布局（与 M-2 一致）。

---

## 4. 浏览器 WebSocket 报文（schema 对齐 node-server）

字段命名对齐 node-server `HmiPointValueDto` / `HmiControlCommandDto` / `HmiBatchPointsRequestDto`，
以便复用 edge-framework 现有前端组件。

### 4.1 服务端 → 浏览器（实时推送，对应 NOTIFY）

```json
{
  "op": "update",
  "pointId": "motor1.speed",
  "id": 123,
  "value": "23.5",
  "quality": "0",
  "ts": 1696500000000000000,
  "sourceTs": 1696500000000000000,
  "type": 2
}
```
- `pointId`：点位名（string），主键，对齐 `HmiPointValueDto.pointId`。
- `id`：RTDB 数值 point_id，便于内部寻址。
- `value`：字符串化值（与 HMI 一致，前端按需解析）。
- `quality`：数值质量码；前端附可读映射（GOOD/BAD/TIMEOUT…）。
- `ts` / `sourceTs`：入库时刻 / 采集时刻（ns）。
- `type`：INDURTDB_TYPE_*。

### 4.2 浏览器 → 服务端（控制 / 查询）

```json
{ "op": "set", "pointId": "motor1.speed", "value": "23.5" }      // 按名设值
{ "op": "set", "id": 123, "value": "23.5" }                      // 按 id 设值
{ "op": "subscribe", "pointIds": ["motor1.speed", "motor2.temp"] } // 订阅（亦支持全部）
{ "op": "list" }                                                 // 拉全量点位清单
```

### 4.3 服务端 → 浏览器（清单响应，对应 LIST）

```json
{
  "op": "list",
  "points": [
    { "id": 123, "name": "motor1.speed", "type": 2, "access": 3 },
    { "id": 124, "name": "motor2.temp",  "type": 1, "access": 1 }
  ]
}
```
- 多连接共享同一后端到 rtdbd 的订阅；后端按浏览器订阅集合做 NOTIFY 多路分发。

---

## 5. 后端模块设计

`tools/rtdb-monitor/`：

```
tools/rtdb-monitor/
├── rtdb_client.py     # UDS 连接 + 协议编解码 + NOTIFY 接收线程（纯 Python）
├── app.py             # FastAPI：REST + WebSocket
├── static/            # 前端单页（index.html + app.js，无构建）
├── requirements.txt   # fastapi, uvicorn, pydantic(可选)
├── tests/
│   ├── test_rtdb_client.py   # 协议编解码单测（mock socket）
│   └── test_e2e.py           # 起真实 rtdbd 的冒烟（复用 run_e2e.sh 模式）
└── README.md
```

### 5.1 `rtdb_client.py`
- 连接 `rtdbd.sock`（AF_UNIX，`SOCK_STREAM`）。
- `request(opcode, payload)`：组 `rtdbd_req_hdr_t`（magic+version+opcode+payload_len）→ 发送 → 收 `rtdbd_resp_hdr_t` → 按 status 返回负载或抛 `RtdbError`。
- 后台线程读 NOTIFY 帧，按 `point_id` 回调给订阅者（写路径不回调；NOTIFY 为服务端推送，无重入问题）。
- 封装高层方法：`get_point(id)`、`list_points(max, offset)`、`write(id, type, value_bits, source_ts)`、`find_by_name(name)`、`subscribe(ids, cb)`。

### 5.2 `app.py`（FastAPI）
- `GET  /api/points` → `LIST`（全量清单）。
- `GET  /api/points/{id}` → `GET`（单点当前值）。
- `POST /api/points/{id}/set` → `WRITE`（body: `{value, sourceTs?}`）。
- `WS   /ws` → 接收浏览器 `subscribe/list/set`，订阅 rtdbd NOTIFY 并向该连接推送 `update`。
- 可选 `Authorization: Bearer <token>` 校验（`--token` 设置时启用）。

### 5.3 安全与暴露
- 默认 `--host 127.0.0.1`（仅本机），`--port 8080`。
- 可选 `--token` 做 Bearer 校验。
- **写值权限最终由 rtdbd 策略决定**：Web 服务以何种 uid 运行，须在 rtdbd 策略文件允许列表内
  （deny by default，无热加载——参见 SDK 手册"已知行为约束"第 7 条）。文档明确此运维约束。

### 5.4 配置
```
python3 -m uvicorn app:app --socket /run/rtdbd.sock --host 127.0.0.1 --port 8080 [--token X]
```
（装配为 `app.py` 启动参数；`--socket` 指定要连的 rtdbd 实例。）

---

## 6. 前端（v1）

- 单页 `index.html` + `app.js`：点位表格（id / 名称 / 类型 / 质量 / 当前值 / 采集时刻）。
- 打开即 `WS /ws` → 发 `list` 拉全量 → 发 `subscribe` 订阅全部 → 实时刷新表格行。
- 点击某行可编辑值并提交 `set`；写入结果以 toast/行高亮反馈。
- 复用 node-server HMI 前端组件的数据契约，故同一套渲染逻辑可消费两类数据源。
- 无框架、无打包：直接 `StaticFiles` 托管，降低部署门槛（契合边缘无头设备）。

---

## 7. 范围（YAGNI）

**v1 包含**
- 枚举全部点位（LIST）
- 实时值推送（WS + NOTIFY）
- 按名 / 按 id 查看当前值（GET）
- 设置点位值（WRITE）
- quality / type / access 展示
- 元数据查看（GET_META；SET_META 可选纳入 v1）

**v1 不含**
- 历史趋势图（RTDB 无持久化）
- 多实例聚合视图
- 用户体系 / 角色 / 审计界面
- 移动端适配

---

## 8. 测试

- **rtdbd（C）**：在 `tests/integration/test_rtdbd_proto_v2.cpp` 增用例覆盖 `OP_GET` / `OP_LIST`
  （值正确、越界 id → `ST_NOT_FOUND`、LIST 分片正确）。
- **Python**：`tests/test_rtdb_client.py` 用 mock socket 验证报文编解码与 status 映射；
  `tests/test_e2e.py` 起真实 rtdbd（复用 `scripts/run_e2e.sh` 的启动/清理模式）做端到端冒烟。
- **CI**：新增 `scripts/run_monitor_smoke.sh`（或并入 `run_e2e.sh`），短时长验证 monitor 起停 + 一次 list/set 往返。

---

## 9. 风险与开放问题

- `LIST` 在 max_points 极大时首屏数据量；采用 `max/offset` 分片 + 前端增量渲染缓解。
- `GET` / `LIST` 走共享内存无锁读，需与 v3.4 既有读路径保持一致的重试范式，避免 TOCTOU。
- rtdbd 策略无热加载：monitor 改 uid 权限须重启 rtdbd（已记为运维约束）。
- 协议版本：新增 opcode 不 bump 主版本；若未来改负载布局需 bump `RTDBD_PROTO_VERSION` 并做协商拒绝。
