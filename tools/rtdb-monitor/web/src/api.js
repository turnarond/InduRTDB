// rtdb-monitor 前端 API 层：REST（fetch）+ WebSocket（实时推送）。
// 所有地址相对当前源，因此 dev（Vite 代理）与生产（FastAPI 托管 dist）通用。

async function rest(path, opts = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...opts
  })
  if (!res.ok) {
    const text = await res.text().catch(() => '')
    throw new Error(`HTTP ${res.status}: ${text}`)
  }
  return res.json()
}

export const api = {
  listPoints: () => rest('/api/points'),
  getPoint: (id) => rest(`/api/points/${id}`),
  setPoint: (id, value) =>
    rest(`/api/points/${id}/set`, { method: 'POST', body: JSON.stringify({ value }) }),
  getMeta: (id) => rest(`/api/meta/${id}`),
  // v3.6 管控写通道：点位 CRUD
  createPoint: (p) => rest('/api/points', { method: 'POST', body: JSON.stringify(p) }),
  deletePoint: (id) => rest(`/api/points/${id}`, { method: 'DELETE' }),
  renamePoint: (id, name) =>
    rest(`/api/points/${id}/rename`, { method: 'POST', body: JSON.stringify({ name }) }),
  // v3.6 命令终端
  runCmd: (cmd) => rest('/api/cmd', { method: 'POST', body: JSON.stringify({ cmd }) })
}

// 轻量 WebSocket 管理：自动重连，统一回调。
export class RtdbSocket {
  constructor({ onMessage, onStatus } = {}) {
    this.onMessage = onMessage || (() => {})
    this.onStatus = onStatus || (() => {})
    this.ws = null
    this.closed = false
    this.connect()
  }
  connect() {
    const proto = location.protocol === 'https:' ? 'wss' : 'ws'
    const ws = new WebSocket(`${proto}://${location.host}/ws`)
    this.ws = ws
    ws.onopen = () => this.onStatus('up')
    ws.onmessage = (ev) => {
      try {
        this.onMessage(JSON.parse(ev.data))
      } catch (e) {
        /* ignore malformed */
      }
    }
    ws.onclose = () => {
      this.onStatus('down')
      if (!this.closed) setTimeout(() => this.connect(), 1500)
    }
    ws.onerror = () => ws.close()
  }
  send(obj) {
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify(obj))
    }
  }
  subscribe(ids) {
    this.send({ op: 'subscribe', pointIds: ids })
  }
  close() {
    this.closed = true
    if (this.ws) this.ws.close()
  }
}
