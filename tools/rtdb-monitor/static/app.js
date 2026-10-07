// InduRTDB Monitor 前端：WebSocket 实时刷新 + 点位值设置（原生 JS，无构建）。
// 报文对齐 node-server 的 HmiPointValueDto（pointId/value/quality/ts），
// 并附带 id / type 便于 RTDB 内部寻址。

const rows = document.getElementById("rows");
const connEl = document.getElementById("conn");
const metaEl = document.getElementById("meta");
const countEl = document.getElementById("count");
const filterEl = document.getElementById("filter");

const byId = new Map();     // id -> <tr>
const byName = new Map();   // name -> <tr>
let points = [];            // 最近一次清单

const Q_MAP = { "0": ["good", "GOOD"], "1": ["bad", "BAD"], "2": ["bad", "TIMEOUT"], "3": ["other", "SUBST"] };

function fmtTs(ns) {
  if (!ns) return "—";
  const d = new Date(Number(ns) / 1e6);
  return d.toLocaleTimeString("zh-CN", { hour12: false }) + "." +
         String(Math.floor(Number(ns) % 1e6)).padStart(6, "0");
}

function makeRow(p) {
  const tr = document.createElement("tr");
  tr.dataset.id = p.id;
  tr.dataset.name = p.name || "";
  tr.innerHTML = `
    <td>${p.id}</td>
    <td class="name">${p.name || "—"}</td>
    <td>${p.typeName || p.type}</td>
    <td><span class="q other">—</span></td>
    <td class="val">—</td>
    <td class="ts">—</td>
    <td class="set"><input placeholder="值" /><button>设</button></td>`;
  tr.querySelector("button").addEventListener("click", () => {
    const v = tr.querySelector("input").value;
    if (v !== "") send({ op: "set", id: p.id, value: v });
  });
  return tr;
}

function upsertRow(p) {
  let tr = byId.get(p.id);
  if (!tr) { tr = makeRow(p); byId.set(p.id, tr); if (p.name) byName.set(p.name, tr); rows.appendChild(tr); }
  return tr;
}

function applyUpdate(u) {
  const tr = byId.get(u.id) || (u.pointId ? byName.get(u.pointId) : null);
  if (!tr) { if (u.id != null) upsertRow({ id: u.id, name: u.pointId, typeName: u.typeName, type: u.type }); return; }
  const q = Q_MAP[u.quality] || ["other", String(u.quality)];
  const qEl = tr.querySelector(".q");
  qEl.className = "q " + q[0]; qEl.textContent = q[1];
  tr.querySelector(".val").textContent = (u.value === undefined ? "—" : String(u.value));
  tr.querySelector(".ts").textContent = fmtTs(u.ts || u.sourceTs);
  tr.classList.remove("flash"); void tr.offsetWidth; tr.classList.add("flash");
}

function applyList(list) {
  points = list;
  byId.clear(); byName.clear(); rows.innerHTML = "";
  for (const p of list) upsertRow(p);
  countEl.textContent = `${list.length} 个点位`;
}

function applyFilter() {
  const f = filterEl.value.trim().toLowerCase();
  for (const tr of byId.values()) {
    const hit = !f || tr.dataset.name.toLowerCase().includes(f) || tr.dataset.id.includes(f);
    tr.style.display = hit ? "" : "none";
  }
}
filterEl.addEventListener("input", applyFilter);
document.getElementById("refresh").addEventListener("click", () => send({ op: "list" }));

// ---- WebSocket ----
let ws;
function send(obj) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj)); }

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => { connEl.textContent = "已连接"; connEl.className = "up"; };
  ws.onclose = () => { connEl.textContent = "未连接"; connEl.className = "down"; setTimeout(connect, 1500); };
  ws.onmessage = (ev) => {
    const m = JSON.parse(ev.data);
    if (m.op === "list") { applyList(m.points || []); applyFilter(); }
    else if (m.op === "update") { applyUpdate(m); }
    else if (m.op === "subscribed") { /* noop */ }
    else if (m.op === "set_ok") { /* 后续由 update 刷新 */ }
    else if (m.op === "error") { console.warn("server error:", m.msg); }
  };
}
connect();
metaEl.textContent = "rtdbd WebSocket 监控台";
