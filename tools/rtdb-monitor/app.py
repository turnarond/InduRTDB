#!/usr/bin/env python3
"""rtdb-monitor Web 后端（FastAPI）。

连接 rtdbd（UDS），提供：
  - REST:  GET  /api/points         枚举全部点位
           GET  /api/points/{id}    读取单点当前值
           POST /api/points/{id}/set 设置点位值（经 rtdbd WRITE，受策略管控）
           GET  /api/meta/{id}       读取点位元数据
  - WS:    /ws                       连接即推送全量清单；点位变更经 NOTIFY 实时推送 update；
           浏览器可发 {op:"set", id, value} / {op:"list"} / {op:"subscribe", pointIds}

安全：默认仅绑 127.0.0.1；设置 RTDB_MONITOR_TOKEN 后需 Bearer/query token。
写值权限最终由 rtdbd 的 SO_PEERCRED + uid 策略决定（web 服务运行 uid 须在策略允许列表）。

用法：
  RTDBD_SOCK=/run/indurtdb/default.sock \\
  RTDB_MONITOR_TOKEN=secret \\
  uvicorn app:app --host 127.0.0.1 --port 8080
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os

from fastapi import FastAPI, Query, Header, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from rtdb_client import RtdbClient, NotifyListener, ST_DENIED, ST_NOT_FOUND, TYPE_NAME

HERE = os.path.dirname(os.path.abspath(__file__))

app = FastAPI(title="InduRTDB Monitor")
SOCK = os.environ.get("RTDBD_SOCK", "/run/indurtdb/default.sock")
TOKEN = os.environ.get("RTDB_MONITOR_TOKEN")  # None 表示不鉴权

api_client: RtdbClient | None = None
listener: NotifyListener | None = None
ws_clients: set[WebSocket] = set()
loop: asyncio.AbstractEventLoop | None = None
name_by_id: dict[int, str] = {}  # id -> point name，用于 WS update.pointId 对齐 HmiPointValueDto


def _authed(token: str | None, authorization: str | None = None) -> bool:
    """鉴权：未设 RTDB_MONITOR_TOKEN 时放行；否则需 query token 或 Authorization: Bearer 命中。"""
    if not TOKEN:
        return True
    if token is not None and token == TOKEN:
        return True
    if authorization:
        scheme, _, val = authorization.partition(" ")
        if (scheme.lower() == "bearer" and val == TOKEN) or authorization == TOKEN:
            return True
    return False


def _broadcast(pid: int, info: dict) -> None:
    """NOTIFY 线程回调：把 update 推给所有 WS 客户端。

    value/quality 字符串化，对齐 node-server HmiPointValueDto（value:String / quality:String），
    便于复用其 HMI 前端组件。
    """
    msg = {
        "op": "update",
        "pointId": name_by_id.get(pid, str(pid)),
        "id": pid,
        "value": str(info["value"]),
        "quality": str(info.get("quality", 0)),
        "ts": info.get("ts", 0),
        "sourceTs": info.get("sourceTs", 0),
        "type": info.get("type"),
    }
    if loop is None:
        return
    dead = []
    for ws in list(ws_clients):
        fut = asyncio.run_coroutine_threadsafe(_safe_send(ws, msg), loop)
        fut.add_done_callback(lambda f, w=ws: dead.append(w) if f.exception() else None)
    for w in dead:
        ws_clients.discard(w)


async def _safe_send(ws: WebSocket, msg: dict) -> None:
    try:
        await ws.send_json(msg)
    except Exception:
        ws_clients.discard(ws)


@app.on_event("startup")
async def _startup() -> None:
    global api_client, listener, loop
    loop = asyncio.get_event_loop()
    api_client = RtdbClient(SOCK).connect()
    # 订阅全量点位（先拿清单，再订阅其 id）
    try:
        pts = api_client.list_points()
        ids = [p["id"] for p in pts]
        name_by_id.clear()
        for p in pts:
            name_by_id[p["id"]] = p["name"]
    except Exception:
        ids = []
    listener = NotifyListener(SOCK, on_notify=_broadcast)
    listener.start()
    if ids:
        listener.subscribe(ids)


@app.on_event("shutdown")
async def _shutdown() -> None:
    if listener:
        listener.stop()
    if api_client:
        api_client.close()


@app.get("/api/points")
def api_points(token: str | None = Query(default=None),
              authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    return api_client.list_points()


@app.get("/api/points/{point_id}")
def api_point(point_id: int, token: str | None = Query(default=None),
             authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    try:
        return api_client.get_point(point_id)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e), "status": "not_found"}, 404


@app.post("/api/points/{point_id}/set")
async def api_set(point_id: int, payload: dict, token: str | None = Query(default=None),
                 authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    value = payload.get("value")
    ptype = payload.get("type")
    if ptype is None:
        # 自动按当前点位类型解析
        try:
            ptype = api_client.get_point(point_id)["type"]
        except Exception:  # noqa: BLE001
            return {"error": "cannot resolve point type"}, 400
    try:
        api_client.write_point(point_id, int(ptype), value)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 400
    return {"ok": True, "id": point_id, "value": value}


@app.get("/api/meta/{point_id}")
def api_meta(point_id: int, token: str | None = Query(default=None),
             authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    try:
        return api_client.get_meta(point_id)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 404


def _deny():
    from fastapi.responses import JSONResponse
    return JSONResponse({"error": "unauthorized"}, status_code=401)


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket, token: str | None = Query(default=None)):
    auth = ws.headers.get("authorization")
    if not _authed(token, auth):
        await ws.close(code=4401)
        return
    await ws.accept()
    ws_clients.add(ws)
    try:
        # 连接即推送全量清单
        try:
            pts = api_client.list_points()
        except Exception:  # noqa: BLE001
            pts = []
        await ws.send_json({"op": "list", "points": pts})
        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            op = msg.get("op")
            if op == "list":
                await ws.send_json({"op": "list", "points": pts})
            elif op == "set":
                await _ws_set(ws, msg)
            elif op == "subscribe":
                ids = msg.get("pointIds") or []
                # 支持按名订阅：解析为 id
                resolved = []
                for x in ids:
                    if isinstance(x, int):
                        resolved.append(x)
                    else:
                        try:
                            resolved.append(api_client.find_by_name(str(x)))
                        except Exception:  # noqa: BLE001
                            pass
                if resolved and listener:
                    listener.subscribe(resolved)
                await ws.send_json({"op": "subscribed", "ids": resolved})
    except WebSocketDisconnect:
        pass
    finally:
        ws_clients.discard(ws)


async def _ws_set(ws: WebSocket, msg: dict) -> None:
    pid = msg.get("id")
    value = msg.get("value")
    if pid is None:
        await ws.send_json({"op": "error", "msg": "missing id"})
        return
    ptype = msg.get("type")
    if ptype is None:
        try:
            ptype = api_client.get_point(int(pid))["type"]
        except Exception:  # noqa: BLE001
            await ws.send_json({"op": "error", "msg": "cannot resolve type"})
            return
    try:
        api_client.write_point(int(pid), int(ptype), value)
        await ws.send_json({"op": "set_ok", "id": int(pid), "value": value})
    except Exception as e:  # noqa: BLE001
        await ws.send_json({"op": "error", "msg": str(e)})


@app.get("/")
def index():
    return FileResponse(os.path.join(HERE, "static", "index.html"))


app.mount("/", StaticFiles(directory=os.path.join(HERE, "static"), html=True), name="static")


def main() -> None:
    import uvicorn
    global SOCK

    parser = argparse.ArgumentParser(description="InduRTDB Web Monitor")
    parser.add_argument("--socket", default=SOCK, help="rtdbd UDS 路径")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    SOCK = args.socket
    os.environ["RTDBD_SOCK"] = args.socket
    uvicorn.run(app, host=args.host, port=args.port)


if __name__ == "__main__":
    main()
