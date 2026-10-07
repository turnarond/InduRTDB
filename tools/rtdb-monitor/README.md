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

- `rtdb_client.py`：UDS 协议客户端（PING/GET/LIST/WRITE/FIND_BY_NAME/GET_META/SUBSCRIBE→NOTIFY），零第三方依赖。
- `app.py`：REST + WebSocket；`/ws` 连接即推全量清单，点位变更经 NOTIFY 实时推送 `update`。
- `static/`：前端单页（无构建，FastAPI 直接托管）。

## 运行

```bash
cd tools/rtdb-monitor
pip install -r requirements.txt

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

## 协议扩展（rtdbd 侧，v3.5）

| opcode | 说明 | 鉴权 |
|---|---|---|
| `OP_GET 10` | 按 id 读当前值（未注册点返回 NOT_FOUND） | 免鉴权 |
| `OP_LIST 11` | 枚举已注册点位（max/offset 分页，按已注册条数计） | 免鉴权 |

`OP_WRITE`（设值）沿用既有写入路径，受 `SO_PEERCRED` + uid 策略管控。

## 测试

```bash
# 协议编解码单测（无需 rtdbd）
python3 tests/test_rtdb_client.py

# 端到端冒烟：启真实 rtdbd + Python 客户端 list/set/get 往返
bash run_smoke.sh
```

## 限制（v1）

- 字符串点位写值受 8B `value_bits` 负载限制，未支持（读值仍可用 `value_str`）。
- 无历史趋势（RTDB 无持久化）、无多实例聚合、无用户体系。
