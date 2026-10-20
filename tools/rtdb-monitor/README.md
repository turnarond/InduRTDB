# rtdb-monitor — InduRTDB Web 监控台

类 Redis Insight 的 RTDB 监控软件：枚举全部点位、实时值推送、按名/id 查看、设置点位值。
Web 仪表盘形态，后端为纯 Python（FastAPI），经 UDS 连接 `rtdbd` 守护进程；浏览器侧 WebSocket
报文在 schema 层对齐 edge-framework node-server 的 `HmiPointValueDto`（统一接口，不耦合 VSOA）。

## 架构

```
浏览器 (原生 JS 单页)  ⇄  WebSocket / HTTP  ⇄  app.py (FastAPI)
                                                  │ UDS 二进制协议（rtdb_client.py 复刻 protocol.h）
                                                  ▼
                                               rtdbd  ⇄  共享内存 (RTDB)
```

- `rtdb_client.py`：UDS 协议客户端（**协议 v3**，PING/GET/LIST/WRITE/FIND_BY_NAME/GET_META/SUBSCRIBE→NOTIFY + 点位 CRUD），零第三方依赖。
- `app.py`：REST + WebSocket；`/ws` 连接即推全量清单，点位变更经 NOTIFY 实时推送 `update`。
- `static/`：前端单页（无构建，FastAPI 直接托管）；`web/`：Vite 前端工程（构建产物 `web/dist`）。

## 运行

```bash
cd tools/rtdb-monitor
pip install -r requirements.txt

# Vite 前端产物 web/dist 未入库，首次运行前构建（缺产物时首页 404）
npm --prefix web ci && npm --prefix web run build

# 依赖一个已启动的 rtdbd 实例（其 socket 路径）
export RTDBD_SOCK=/run/indurtdb/default.sock
# 可选：设置后需 token 鉴权
export RTDB_MONITOR_TOKEN=secret

python3 -m uvicorn app:app --host 127.0.0.1 --port 8080
# 浏览器打开 http://127.0.0.1:8080
```

或用参数：`python3 app.py --socket <rtdbd.sock> --host 127.0.0.1 --port 8080`

## 安全

- 默认仅绑 `127.0.0.1`；设 `RTDB_MONITOR_TOKEN` 后 WS/REST 需 `?token=...` 或 `Authorization: Bearer`。
- **写值权限最终由 rtdbd 策略决定**：web 服务以何种 uid 运行，须在 rtdbd 策略文件允许列表内
  （deny by default，无热加载——见 SDK 手册「已知行为约束」第 7 条）。

## 协议扩展（rtdbd 侧，协议 v3）

| opcode | 说明 | 鉴权 |
|---|---|---|
| `OP_GET 10` | 按 id 读当前值（未注册点返回 NOT_FOUND） | 免鉴权 |
| `OP_LIST 11` | 枚举已注册点位（`max+offset+partition` 分页；`partition=0xFFFFFFFF` 列出全部，否则仅该分区；响应项含 `partition`） | 免鉴权 |
| `OP_FIND_BY_NAME 7` | 在 `(partition, name)` 内按名查 id | 免鉴权 |
| `OP_CREATE/DELETE/RENAME_POINT 12/13/14` | 点位 CRUD（请求体末尾带 `partition`） | 须鉴权 |
| `OP_SELECT 17` | 连接级默认分区（Redis SELECT 语义） | 须鉴权 |

`OP_WRITE`（设值）沿用既有写入路径，受 `SO_PEERCRED` + uid 策略管控。
**协议主版本为 3**；v2 及更早客户端被 rtdbd 拒绝（`rtdb_client.py` 已同步 v3）。

## REST API（v3.9 T7 分区）

| 端点 | 说明 |
|---|---|
| `GET /api/points` | 枚举全部点位（列全部分区，v3.8 行为不变） |
| `GET /api/points?partition=N` | 仅返回分区 N 的点位 |
| `GET /api/partitions` | 已注册点位的分区列表（升序去重）：`{"partitions": [0, 1, ...]}` |

`/api/points` 每项新增 `partition` 字段（纯增量）；`/ws` 与内部清单仍列出全部。

## 测试

```bash
# 协议编解码单测（无需 rtdbd）
python3 tests/test_rtdb_client.py

# 端到端冒烟：启真实 rtdbd + Python 客户端 list/set/get 往返
bash run_smoke.sh

# 端到端（REST+WS+CRUD+日志）：启真实 rtdbd + FastAPI TestClient
python3 tests/test_e2e.py

# 分区端到端：/api/partitions + 按分区过滤 + 前端源码契约
python3 tests/test_partitions.py
```

## 限制（v1）

- 字符串点位写值受 8B `value_bits` 负载限制，未支持（读值仍可用 `value_str`）。
- 无历史趋势（RTDB 无持久化）、无多实例聚合、无用户体系。
