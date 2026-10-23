#!/usr/bin/env python3
"""rtdb-monitor Web 后端（FastAPI）。

连接 rtdbd（UDS），提供：
  - REST:  GET  /api/points         枚举全部点位
           GET  /api/points/{id}    读取单点当前值
           POST /api/points/{id}/set 设置点位值（经 rtdbd WRITE，受策略管控）
           GET  /api/meta/{id}       读取点位元数据
  - WS:    /ws                       连接即推送全量清单；点位变更经 NOTIFY 实时推送 update；
           浏览器可发 {op:"set", id, value} / {op:"list"} / {op:"set_subs", ids}
           或兼容旧前端的 {op:"subscribe", pointIds}

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
import threading

from fastapi import FastAPI, Query, Header, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse
from fastapi.staticfiles import StaticFiles

from rtdb_client import (RtdbClient, NotifyListener, probe_set_subs,
                         probe_caps, RTDBD_LEGACY_SET_SUBS_MAX)

HERE = os.path.dirname(os.path.abspath(__file__))

app = FastAPI(title="InduRTDB Monitor")
SOCK = os.environ.get("RTDBD_SOCK", "/run/indurtdb/default.sock")
TOKEN = os.environ.get("RTDB_MONITOR_TOKEN")  # None 表示不鉴权

api_client: RtdbClient | None = None
ws_clients: set[WebSocket] = set()
ws_listeners: dict[WebSocket, NotifyListener] = {}
loop: asyncio.AbstractEventLoop | None = None
name_by_id: dict[int, str] = {}  # id -> point name，用于 WS update.pointId 对齐 HmiPointValueDto
registry: list[dict] = []
registry_lock = threading.Lock()
supports_set_subs = False
supports_caps = False
max_subs = RTDBD_LEGACY_SET_SUBS_MAX


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


def _notify_msg(pid: int, info: dict) -> dict:
    """把 NOTIFY 回调信息转成前端 update JSON（value/quality 字符串化）。"""
    return {
        "op": "update",
        "pointId": name_by_id.get(pid, str(pid)),
        "id": pid,
        "value": str(info["value"]),
        "quality": str(info["quality"]) if "quality" in info else None,
        "ts": info.get("ts", 0),
        "sourceTs": info.get("sourceTs", 0),
        "type": info.get("type"),
    }


def _make_notify_callback(ws: WebSocket):
    """每个 WS 独占 NotifyListener；回调只投递到本 WS 的事件循环。"""
    def _on_notify(pid: int, info: dict) -> None:
        if loop is None:
            return
        asyncio.run_coroutine_threadsafe(_safe_send(ws, _notify_msg(pid, info)), loop)
    return _on_notify


def _make_snapshot_callback(ws: WebSocket):
    """快照屏障回调：通知前端本轮 SET_SUBS 的全量快照已结束。"""
    def _on_done(info: dict) -> None:
        if loop is None:
            return
        msg = {"op": "snapshot_done", "count": info.get("count", 0),
               "flags": info.get("flags", 0)}
        asyncio.run_coroutine_threadsafe(_safe_send(ws, msg), loop)
    return _on_done


async def _safe_send(ws: WebSocket, msg: dict) -> None:
    try:
        await ws.send_json(msg)
    except Exception:
        ws_clients.discard(ws)


def _connect_and_probe() -> bool:
    global api_client, supports_set_subs, supports_caps, max_subs
    candidate = RtdbClient(SOCK)
    try:
        candidate.connect()
        caps = probe_caps(candidate)
        if caps is not None:
            supports_caps = True
            supported = True
            max_subs = int(caps.get("max_subs_per_conn") or RTDBD_LEGACY_SET_SUBS_MAX)
        else:
            # CAPS 探测可能被旧服务端以 BAD_REQUEST + 断链拒绝；重连后再探 SET_SUBS。
            supports_caps = False
            candidate.close()
            candidate = RtdbClient(SOCK).connect()
            supported = probe_set_subs(candidate)
            max_subs = RTDBD_LEGACY_SET_SUBS_MAX
        if not supported:
            candidate.close()
            candidate = RtdbClient(SOCK).connect()
    except Exception:  # noqa: BLE001
        candidate.close()
        return False
    old = api_client
    api_client = candidate
    supports_set_subs = supported
    if old is not None:
        old.close()
    return True


async def _registry_retry() -> None:
    """rtdbd 未就绪时后台重试；不阻塞 Web 启动。"""
    for _ in range(300):
        await asyncio.sleep(0.2)
        if api_client is None or not api_client.connected:
            if not _connect_and_probe():
                continue
        if _refresh_registry():
            return
        if api_client is not None:
            api_client.close()


@app.on_event("startup")
async def _startup() -> None:
    global loop
    loop = asyncio.get_running_loop()
    if not _connect_and_probe():
        print(f"[warn] rtdb-monitor: 初始连接 rtdb 失败（{SOCK}），API 暂不可用",
              flush=True)
    if not _refresh_registry():
        asyncio.create_task(_registry_retry())


@app.on_event("shutdown")
async def _shutdown() -> None:
    for l in list(ws_listeners.values()):
        l.stop()
    ws_listeners.clear()
    if api_client:
        api_client.close()


def _registry_snapshot() -> list[dict]:
    with registry_lock:
        return list(registry)


def _ensure_registry() -> list[dict]:
    pts = _registry_snapshot()
    if pts:
        return pts
    _refresh_registry()
    return _registry_snapshot()


@app.get("/api/points")
def api_points(token: str | None = Query(default=None),
              authorization: str | None = Header(default=None),
              partition: int | None = Query(default=None),
              offset: int | None = Query(default=None, ge=0),
              limit: int | None = Query(default=None, ge=1, le=500),
              q: str | None = Query(default=None),
              point_type: str | None = Query(default=None, alias="type"),
              access: int | None = Query(default=None)):
    if not _authed(token, authorization):
        return _deny()

    pts = _ensure_registry()
    if partition is not None:
        pts = [p for p in pts if p.get("partition", 0) == partition]
    if q:
        needle = q.lower()
        pts = [p for p in pts if needle in str(p.get("name", "")).lower()]
    if point_type is not None:
        want = str(point_type)
        pts = [p for p in pts
               if want in (str(p.get("type")), str(p.get("typeName")))]
    if access is not None:
        pts = [p for p in pts if p.get("access") == access]
    pts = sorted(pts, key=lambda p: p.get("id", 0))

    page_mode = any(v is not None for v in (offset, limit, q, point_type, access))
    if not page_mode:
        return pts

    o = offset or 0
    l = limit if limit is not None else 50
    total = len(pts)
    return {"points": pts[o:o + l], "total": total, "offset": o, "limit": l}


@app.get("/api/partitions")
def api_partitions(token: str | None = Query(default=None),
                   authorization: str | None = Header(default=None)):
    """已注册点位的分区列表（升序去重），derive 自注册表缓存。"""
    if not _authed(token, authorization):
        return _deny()
    try:
        pts = _ensure_registry()
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e)}, 400)
    return {"partitions": sorted({p.get("partition", 0) for p in pts})}


@app.get("/api/points/{point_id}")
def api_point(point_id: int, token: str | None = Query(default=None),
             authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    try:
        return api_client.get_point(point_id)
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e), "status": "not_found"}, 404)


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
            return _err({"error": "cannot resolve point type"}, 400)
    try:
        api_client.write_point(point_id, int(ptype), value)
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e)}, 400)
    return {"ok": True, "id": point_id, "value": value}


@app.get("/api/meta/{point_id}")
def api_meta(point_id: int, token: str | None = Query(default=None),
             authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    try:
        return api_client.get_meta(point_id)
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e)}, 404)


def _err(payload: dict, status_code: int) -> JSONResponse:
    """错误响应。新版 FastAPI 不再把 (body, status) 元组解释为状态码，
    必须显式返回 JSONResponse，否则错误会被序列化成 200 + JSON 数组。"""
    return JSONResponse(payload, status_code=status_code)


def _deny():
    return JSONResponse({"error": "unauthorized"}, status_code=401)


# ---- v3.6 管控写通道：点位 CRUD ----

def _refresh_registry() -> bool:
    """CRUD/启动时刷新注册表缓存；失败返回 False，由调用方决定重试。"""
    if api_client is None:
        return False
    try:
        pts = api_client.list_points()
    except Exception:  # noqa: BLE001
        return False
    with registry_lock:
        registry[:] = pts
        name_by_id.clear()
        for p in pts:
            name_by_id[p["id"]] = p["name"]
    return True


@app.post("/api/points")
def api_create(payload: dict, token: str | None = Query(default=None),
               authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    pid = payload.get("id")
    name = payload.get("name")
    if pid is None or not name:
        return _err({"error": "id and name are required"}, 400)
    # 面向用户的 ID 从 1 开始分配；0 仍是底层合法索引（配置/历史点位），
    # 但不在监控台中作为「新建点位」的入口。
    if isinstance(pid, bool) or not isinstance(pid, int) or pid < 1:
        return _err({"error": "id 必须为 ≥ 1 的整数（0 保留给历史/配置点位）"}, 400)
    try:
        api_client.create_point(pid, name, payload.get("type", "int32"),
                                payload.get("access", 3))
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e)}, 400)
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
        return _err({"error": str(e)}, 404)
    _refresh_registry()
    return {"ok": True, "id": point_id}


@app.post("/api/points/{point_id}/rename")
def api_rename(point_id: int, payload: dict, token: str | None = Query(default=None),
               authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    name = payload.get("name")
    if not name:
        return _err({"error": "name is required"}, 400)
    try:
        api_client.rename_point(point_id, name)
    except Exception as e:  # noqa: BLE001
        return _err({"error": str(e)}, 400)
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
        return _err({"error": str(e)}, 400)


@app.post("/api/cmd")
def api_cmd(payload: dict, token: str | None = Query(default=None),
            authorization: str | None = Header(default=None)):
    if not _authed(token, authorization):
        return _deny()
    line = str(payload.get("cmd", ""))
    return {"cmd": line, "output": _run_cmd(line)}


def _resolve_ws_ids(raw) -> list[int]:
    out: list[int] = []
    seen: set[int] = set()
    for x in raw or []:
        try:
            if isinstance(x, bool):
                continue
            if isinstance(x, int):
                pid = x
            elif isinstance(x, str) and x.lstrip("-").isdigit():
                pid = int(x)
            else:
                pid = api_client.find_by_name(str(x))
        except Exception:  # noqa: BLE001
            continue
        if pid not in seen:
            seen.add(pid)
            out.append(pid)
    return out


@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket, token: str | None = Query(default=None)):
    auth = ws.headers.get("authorization")
    if not _authed(token, auth):
        await ws.close(code=4401)
        return
    await ws.accept()
    if len(ws_clients) >= 32:
        await ws.close(code=1013)
        return

    ws_clients.add(ws)
    ws_listener = NotifyListener(SOCK, on_notify=_make_notify_callback(ws),
                                 on_snapshot_done=_make_snapshot_callback(ws))
    ws_listeners[ws] = ws_listener
    try:
        ws_listener.start(negotiate=supports_caps)
        pts = _ensure_registry()
        await ws.send_json({"op": "list", "points": pts})

        # 旧前端兼容：默认只订阅首屏前 32 点；新前端随后发 set_subs 整组替换。
        initial = [p["id"] for p in pts[:32]]
        if initial:
            ws_listener.subscribe(initial)
        if not supports_set_subs:
            await ws.send_json({"type": "legacy", "page_size": 32})

        while True:
            raw = await ws.receive_text()
            try:
                msg = json.loads(raw)
            except json.JSONDecodeError:
                continue
            op = msg.get("op")
            if op == "list":
                await ws.send_json({"op": "list", "points": _ensure_registry()})
            elif op == "set":
                await _ws_set(ws, msg)
            elif op == "set_subs":
                ids = _resolve_ws_ids(
                    msg.get("ids") or msg.get("pointIds") or []
                )[:ws_listener.max_subs]
                if supports_set_subs:
                    ws_listener.set_subs(ids)
                else:
                    ids = ws_listener.replace_subs_legacy(ids)
                    await ws.send_json({"type": "legacy", "page_size": 32,
                                        "ids": ids})
                await ws.send_json({"op": "subscribed", "ids": ids})
            elif op == "subscribe":
                ids = _resolve_ws_ids(
                    msg.get("ids") or msg.get("pointIds") or []
                )[:ws_listener.max_subs]
                if supports_set_subs:
                    ws_listener.subscribe(ids)
                else:
                    ids = ws_listener.replace_subs_legacy(ids)
                    await ws.send_json({"type": "legacy", "page_size": 32,
                                        "ids": ids})
                await ws.send_json({"op": "subscribed", "ids": ids})
            elif op == "ping":
                await ws.send_json({"type": "pong"})
    except WebSocketDisconnect:
        pass
    except Exception as e:  # noqa: BLE001
        try:
            await ws.send_json({"op": "error", "msg": str(e)})
        except Exception:  # noqa: BLE001
            pass
    finally:
        ws_listeners.pop(ws, None)
        ws_clients.discard(ws)
        ws_listener.stop()


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
