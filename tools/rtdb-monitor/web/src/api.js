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
  // v3.10：服务端分页。返回 {points,total,offset,limit}；无分页参数时兼容旧数组。
  fetchPoints: async ({ partition, offset, limit, q, type, access } = {}) => {
    const qs = new URLSearchParams()
    const put = (k, v) => {
      if (v !== undefined && v !== null && v !== '') qs.set(k, String(v))
    }
    put('partition', partition)
    put('offset', offset)
    put('limit', limit)
    put('q', q)
    put('type', type)
    put('access', access)
    const data = await rest(`/api/points${qs.toString() ? `?${qs}` : ''}`)
    if (Array.isArray(data)) {
      // 旧后端忽略分页参数并返回全量数组：在前端切片并标记 legacy，触发 32 点/页降级。
      const o = Number(offset) || 0
      const l = limit === undefined || limit === null ? data.length : Number(limit)
      return { points: data.slice(o, o + l), total: data.length, offset: o, limit: l, legacy: true }
    }
    return data
  },
  // v3.9 T7：分区过滤（无分页旧接口；保留给分区视图/兼容调用）。
  listPoints: (partition) =>
    rest(partition === undefined || partition === null ? '/api/points' : `/api/points?partition=${partition}`),
  // v3.9 T7：已注册分区列表（升序去重）。
  listPartitions: () => rest('/api/partitions'),
  getPoint: (id) => rest(`/api/points/${id}`),
  setPoint: (id, value) =>
    rest(`/api/points/${id}/set`, { method: 'POST', body: JSON.stringify({ value }) }),
  getMeta: (id) => rest(`/api/meta/${id}`),
  // v3.6 管控写通道：点位 CRUD
  createPoint: (p) => rest('/api/points', { method: 'POST', body: JSON.stringify(p) }),
  deletePoint: (id) => rest(`/api/points/${id}`, { method: 'DELETE' }),
  renamePoint: (id, name) =>
    rest(`/api/points/${id}/rename`, { method: 'POST', body: JSON.stringify({ name }) }),
  // v3.6 运行日志
  getLogs: (maxN = 0) => rest(`/api/logs?max_n=${maxN}`),
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
  setSubs(ids) {
    this.send({ op: 'set_subs', ids })
  }
  close() {
    this.closed = true
    if (this.ws) this.ws.close()
  }
}
