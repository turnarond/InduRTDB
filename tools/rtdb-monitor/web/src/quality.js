// 质量码渲染（INDURTDB_QUALITY_*）。后端 DTO 的 quality 是字符串（如 "0"），
// 初始 REST 读取是数字，这里统一归一化后再判断，避免 "0" === 0 恒为 false。
const BASE_LABEL = {
  0: 'Good',
  1: 'Bad',
  2: 'Timeout',
  3: 'Substituted',
  4: 'Uncertain',
  5: 'Not Init',
  6: 'Out Of Service',
  7: 'Comm Failure',
  8: 'Sensor Failure',
  9: 'Last Known',
  10: 'Config Error'
}
const BASE_TAG = {
  0: 'success',
  1: 'danger',
  2: 'warning',
  3: 'warning',
  4: 'warning',
  5: 'info',
  6: 'info',
  7: 'danger',
  8: 'danger',
  9: 'info',
  10: 'danger'
}

export function normalizeQuality(q) {
  if (q === null || q === undefined || q === '') return null
  const n = Number(q)
  return Number.isFinite(n) ? n : null
}

export function qualityLabel(q) {
  const n = normalizeQuality(q)
  if (n === null) return '—'
  const base = n & 0x0f
  const limit = (n >> 4) & 0x03
  const suffix = limit === 1 ? ' · Lo' : limit === 2 ? ' · Hi' : limit === 3 ? ' · Const' : ''
  return (BASE_LABEL[base] || `Code ${base}`) + suffix
}

export function qualityTagType(q) {
  const n = normalizeQuality(q)
  if (n === null) return 'info'
  return BASE_TAG[n & 0x0f] || 'info'
}
