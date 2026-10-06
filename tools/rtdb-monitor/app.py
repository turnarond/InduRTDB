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
import datetime
import json
import os

from fastapi import FastAPI, Query, Header, WebSocket, WebSocketDisconnect
from fastapi.staticfiles import StaticFiles

from rtdb_client import RtdbClient, NotifyListener

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
    api_client = RtdbClient(SOCK)
    try:
        api_client.connect()
    except Exception as e:  # noqa: BLE001
        # rtdb 尚未就绪：Web 仍启动，API 调用会返回错误；rtdbd 就绪后自动恢复。
        print(f"[warn] rtdb-monitor: 初始连接 rtdb 失败（{e}），API 暂不可用", flush=True)
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
def api_set(point_id: int, payload: dict, token: str | None = Query(default=None),
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


# ---- v3.6 管控写通道：点位 CRUD ----

def _refresh_registry() -> None:
    """CRUD 后刷新 id→name 缓存，并让监听线程订阅新增 id（含改名后的清单同步）。"""
    try:
        pts = api_client.list_points()
    except Exception:  # noqa: BLE001
        return
    name_by_id.clear()
    for p in pts:
        name_by_id[p["id"]] = p["name"]
    if listener:
        try:
            listener.subscribe([p["id"] for p in pts])
        except Exception:  # noqa: BLE001
            pass


@app.post("/api/points")
def api_create(payload: dict, token: str | None = Query(default=None),
               authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    pid = payload.get("id")
    name = payload.get("name")
    if pid is None or not name:
        return {"error": "id and name are required"}, 400
    try:
        api_client.create_point(pid, name, payload.get("type", "int32"),
                                payload.get("access", 3))
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 400
    _refresh_registry()
    return {"ok": True, "id": pid, "name": name}


@app.delete("/api/points/{point_id}")
def api_delete(point_id: int, token: str | None = Query(default=None),
               authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    try:
        api_client.delete_point(point_id)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 404
    _refresh_registry()
    return {"ok": True, "id": point_id}


@app.post("/api/points/{point_id}/rename")
def api_rename(point_id: int, payload: dict, token: str | None = Query(default=None),
               authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    name = payload.get("name")
    if not name:
        return {"error": "name is required"}, 400
    try:
        api_client.rename_point(point_id, name)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 400
    _refresh_registry()
    return {"ok": True, "id": point_id, "name": name}


# ---- v3.6 命令终端（类 Redis CLI 的最小命令集） ----

def _resolve_id(tok: str) -> int:
    """把 id 字面量或点位名解析为 id（名为非数字时按名查找）。"""
    s = str(tok).strip()
    if s.lstrip("-").isdigit():
        return int(s)
    return api_client.find_by_name(s)


def _fmt_log(e: dict) -> str:
    """把日志条目格式化为一行（墙上时钟转本地可读时间）。"""
    try:
        t = datetime.datetime.fromtimestamp(e["ts"] / 1e9).strftime("%H:%M:%S.%f")[:-3]
    except Exception:  # noqa: BLE001
        t = str(e["ts"])
    return f"{t} [{e.get('levelName', '?')}] {e.get('msg', '')}"


def _run_cmd(line: str) -> list[str]:
    """执行一条终端命令，返回输出行。失败统一以 ERR: 前缀返回，不抛异常。"""
    parts = line.strip().split()
    if not parts:
        return []
    cmd, args = parts[0].lower(), parts[1:]
    try:
        if cmd == "help":
            return [
                "可用命令：",
                "  ping                              测试连通",
                "  list                              列出全部点位",
                "  find <name>                       按名查 id",
                "  get <id|name>                     读当前值",
                "  set <id|name> <value>             设值",
                "  meta <id|name>                    读元数据",
                "  create <id> <name> [type] [access] 新建点位",
                "  del <id|name>                     删除点位",
                "  rename <id|name> <newname>        重命名点位",
                "  logs [n]                          查看运行日志（默认全部，上限 128）",
            ]
        if cmd == "ping":
            return ["PONG" if api_client.ping() else "ERR: ping failed"]
        if cmd == "list":
            pts = api_client.list_points()
            return [f"{p['id']}\t{p['name']}\t{p['typeName']}" for p in pts] or ["(空)"]
        if cmd == "find":
            if not args:
                return ["用法: find <name>"]
            return [str(api_client.find_by_name(args[0]))]
        if cmd == "meta":
            if not args:
                return ["用法: meta <id|name>"]
            m = api_client.get_meta(_resolve_id(args[0]))
            return [f"eurMin={m['eurMin']} eurMax={m['eurMax']} "
                    f"deadband={m['deadband']} flags={m['flags']}"]
        if cmd == "get":
            if not args:
                return ["用法: get <id|name>"]
            p = api_client.get_point(_resolve_id(args[0]))
            return [f"{p['value']}  (quality={p['quality']}, ts={p['ts']})"]
        if cmd == "set":
            if len(args) < 2:
                return ["用法: set <id|name> <value>"]
            pid = _resolve_id(args[0])
            api_client.write_point(pid, api_client.get_point(pid)["type"], args[1])
            return ["OK"]
        if cmd == "create":
            if len(args) < 2:
                return ["用法: create <id> <name> [type] [access]"]
            api_client.create_point(int(args[0]), args[1],
                                    args[2] if len(args) > 2 else "int32",
                                    int(args[3]) if len(args) > 3 else 3)
            _refresh_registry()
            return ["OK"]
        if cmd in ("del", "delete"):
            if not args:
                return ["用法: del <id|name>"]
            api_client.delete_point(_resolve_id(args[0]))
            _refresh_registry()
            return ["OK"]
        if cmd == "rename":
            if len(args) < 2:
                return ["用法: rename <id|name> <newname>"]
            api_client.rename_point(_resolve_id(args[0]), args[1])
            _refresh_registry()
            return ["OK"]
        if cmd == "logs":
            n = int(args[0]) if args else 0
            entries = api_client.get_logs(n)
            return [_fmt_log(e) for e in entries] or ["(无日志)"]
        return [f"未知命令: {cmd}（输入 help 查看可用命令）"]
    except Exception as e:  # noqa: BLE001
        return [f"ERR: {e}"]


@app.get("/api/logs")
def api_logs(max_n: int = 0, token: str | None = Query(default=None),
             authorization: str | None = Header(default=None)):
    """rtdbd 运行日志（最旧→最新）。max_n=0 表示全部（服务端上限 128 条）。"""
    if not _authed(token, authorization):
        return _deny()
    try:
        return api_client.get_logs(max_n)
    except Exception as e:  # noqa: BLE001
        return {"error": str(e)}, 400


@app.post("/api/cmd")
def api_cmd(payload: dict, token: str | None = Query(default=None),
            authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    line = str(payload.get("cmd", ""))
    return {"cmd": line, "output": _run_cmd(line)}


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


# 优先托管 Vite 构建产物 web/dist；未构建时回退到原生 static/。
WEB_DIST = os.path.join(HERE, "web", "dist")
WEB_STATIC = os.path.join(HERE, "static")
WEB_DIR = WEB_DIST if os.path.isdir(WEB_DIST) else WEB_STATIC

app.mount("/", StaticFiles(directory=WEB_DIR, html=True), name="web")


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
