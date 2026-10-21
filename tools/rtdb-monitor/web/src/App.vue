<template>
  <el-container class="root">
    <el-aside width="200px" class="aside">
      <div class="logo">InduRTDB</div>
      <el-menu :default-active="view" @select="onMenu">
        <el-menu-item index="points">
          <el-icon><Histogram /></el-icon><span>点位监控</span>
        </el-menu-item>
        <el-menu-item index="partitions">
          <el-icon><Collection /></el-icon><span>点位分区</span>
        </el-menu-item>
        <el-menu-item index="terminal">
          <el-icon><Monitor /></el-icon><span>命令终端</span>
        </el-menu-item>
        <el-menu-item index="logs">
          <el-icon><Document /></el-icon><span>运行日志</span>
        </el-menu-item>
      </el-menu>
    </el-aside>

    <el-container>
      <el-header class="header">
        <div class="title">RTDB 监控台</div>
        <div class="spacer" />
        <span class="conn">
          <span class="conn-dot" :class="status" />
          {{ status === 'up' ? '已连接' : '未连接' }}
        </span>
        <el-button text :icon="Refresh" @click="refresh">刷新</el-button>
        <el-switch
          v-model="isDark"
          inline-prompt
          active-text="暗"
          inactive-text="亮"
          @change="toggleDark"
        />
      </el-header>

      <el-main class="main">
        <el-alert
          v-if="legacyMode"
          class="legacy-alert"
          type="warning"
          :closable="false"
          show-icon
          :title="`旧版 rtdbd 不支持批量订阅，已降级为每页 ${legacyPageSize} 点`"
        />
        <PointTable
          v-if="view === 'points'"
          :points="points"
          :values="values"
          :total="total"
          :page="page"
          :page-size="pageSize"
          :page-sizes="pageSizes"
          :type-options="typeNames"
          @row-click="openPoint"
          @create="onCreate"
          @rename="onRename"
          @delete="onDelete"
          @page-change="onPageChange"
          @page-size-change="onPageSizeChange"
          @filter="onFilter"
        />
        <div v-else-if="view === 'partitions'" class="partition-view">
          <el-form :inline="true" class="filter-bar">
            <el-form-item label="分区">
              <el-select
                v-model="selectedPartition"
                placeholder="请选择分区"
                style="width: 160px"
                @change="loadPartitionPoints"
              >
                <el-option v-for="p in partitions" :key="p" :label="`分区 ${p}`" :value="p" />
              </el-select>
            </el-form-item>
            <el-form-item>
              <span class="count">共 {{ total }} 个点位</span>
            </el-form-item>
          </el-form>
          <PointTable
            v-if="selectedPartition !== null && selectedPartition !== undefined"
            :points="points"
            :values="values"
            :total="total"
            :page="page"
            :page-size="pageSize"
            :page-sizes="pageSizes"
            :type-options="typeNames"
            @row-click="openPoint"
            @create="onCreate"
            @rename="onRename"
            @delete="onDelete"
            @page-change="onPageChange"
            @page-size-change="onPageSizeChange"
            @filter="onFilter"
          />
          <div v-else class="placeholder">
            <el-result icon="info" title="选择分区" sub-title="请从上方下拉选择一个分区以查看该分区点位。" />
          </div>
        </div>
        <TerminalView v-else-if="view === 'terminal'" @changed="refresh" />
        <LogsView v-else-if="view === 'logs'" />
        <div v-else class="placeholder">
          <el-result icon="info" :title="placeholderTitle" sub-title="该功能规划中，需要 rtddb 后端协议扩展（Phase 2）。">
            <template #extra>
              <el-tag type="info">{{ placeholderHint }}</el-tag>
            </template>
          </el-result>
        </div>
      </el-main>
    </el-container>

    <PointDetailDrawer
      v-model="drawerVisible"
      :point="selected"
      :live="selected ? values[selected.id] : null"
      :history="history"
      :meta="meta"
      @set="onSet"
    />

    <el-dialog v-model="createVisible" title="新建点位" width="460px">
      <el-form :model="form" label-width="80px">
        <el-form-item label="ID">
          <el-input-number v-model="form.id" :min="1" :max="4294967295" />
          <div class="field-hint">自动从 1 开始分配下一个空闲 ID（0 保留给历史/配置点位）</div>
        </el-form-item>
        <el-form-item label="名称">
          <el-input v-model="form.name" placeholder="如 AHU_02.Supply_Temp" />
        </el-form-item>
        <el-form-item label="类型">
          <el-select v-model="form.type">
            <el-option v-for="t in typeNames" :key="t" :label="t" :value="t" />
          </el-select>
        </el-form-item>
        <el-form-item label="权限">
          <el-select v-model="form.access">
            <el-option label="只读" :value="1" />
            <el-option label="读写" :value="3" />
          </el-select>
        </el-form-item>
      </el-form>
      <template #footer>
        <el-button @click="createVisible = false">取消</el-button>
        <el-button type="primary" @click="doCreate">创建</el-button>
      </template>
    </el-dialog>
  </el-container>
</template>

<script setup>
import { ref, reactive, onMounted, onBeforeUnmount, computed } from 'vue'
import { Refresh, Histogram, Collection, Monitor, Document } from '@element-plus/icons-vue'
import { api, RtdbSocket } from './api.js'
import PointTable from './components/PointTable.vue'
import PointDetailDrawer from './components/PointDetailDrawer.vue'
import TerminalView from './components/TerminalView.vue'
import LogsView from './components/LogsView.vue'
import { ElMessage, ElMessageBox } from 'element-plus'

const view = ref('points')
const status = ref('down')
const points = ref([]) // 当前页
const allPoints = ref([]) // WS list 全量，仅用于分配新建 ID
const total = ref(0)
const page = ref(1)
const pageSize = ref(50)
const legacyMode = ref(false)
const legacyPageSize = ref(32)
const pageSizes = computed(() => (legacyMode.value ? [legacyPageSize.value] : [10, 20, 32, 50, 100]))
const filters = reactive({ q: '', type: '', access: null })
const values = reactive({}) // id -> {value, quality, ts}
const selected = ref(null)
const drawerVisible = ref(false)
const history = ref([])
const meta = ref(null)
const isDark = ref(false)

let socket = null

// 其它未实现视图的占位信息（partitions 已于 v3.9 T7 落地为真实视图）。
const placeholderMap = {}
const placeholderTitle = computed(() => (placeholderMap[view.value] ? placeholderMap[view.value][0] : ''))
const placeholderHint = computed(() => (placeholderMap[view.value] ? placeholderMap[view.value][1] : ''))

// ---- v3.9 T7 分区视图 ----
const partitions = ref([])
const selectedPartition = ref(null)

function onMenu(index) {
  view.value = index
  if (index === 'points') {
    page.value = 1
    loadPage()
  } else if (index === 'partitions') {
    if (partitions.value.length === 0) loadPartitions()
    if (selectedPartition.value !== null && selectedPartition.value !== undefined) {
      page.value = 1
      loadPage()
    } else {
      points.value = []
      total.value = 0
    }
  }
}

async function loadPartitions() {
  try {
    const d = await api.listPartitions()
    partitions.value = d.partitions || []
  } catch (e) {
    /* ignore */
  }
}

async function loadPartitionPoints() {
  page.value = 1
  await loadPage()
}

// ---- v3.10 服务端分页 ----
let loadSeq = 0
let resyncTimer = null

function activePartition() {
  return view.value === 'partitions' ? selectedPartition.value : null
}

async function loadPage() {
  const part = activePartition()
  if (view.value === 'partitions' && (part === null || part === undefined)) {
    points.value = []
    total.value = 0
    return
  }
  const seq = ++loadSeq
  try {
    const data = await api.fetchPoints({
      partition: part,
      offset: (page.value - 1) * pageSize.value,
      limit: pageSize.value,
      ...filters
    })
    if (seq !== loadSeq) return
    if (data.legacy) {
      legacyMode.value = true
      legacyPageSize.value = 32
      if (pageSize.value !== 32) {
        pageSize.value = 32
        page.value = 1
        return loadPage()
      }
    }
    points.value = data.points || []
    total.value = data.total ?? points.value.length
    const maxPage = Math.max(1, Math.ceil(total.value / pageSize.value))
    if (page.value > maxPage) {
      page.value = maxPage
      return loadPage()
    }
    await loadValues(points.value)
    syncSubs()
  } catch (e) {
    /* ignore */
  }
}

function syncSubs() {
  if (!socket) return
  socket.setSubs(points.value.map((p) => p.id))
}

function onPageChange(p) {
  page.value = p
  loadPage()
}

function onPageSizeChange(size) {
  pageSize.value = size
  page.value = 1
  loadPage()
}

function onFilter(f) {
  filters.q = f.q
  filters.type = f.type
  filters.access = f.access
  page.value = 1
  loadPage()
}

// ---- v3.6 点位 CRUD ----
const createVisible = ref(false)
const form = ref({ id: 1, name: '', type: 'int32', access: 3 })
const typeNames = ['bool', 'int32', 'double', 'string', 'int64', 'uint32', 'float']

function nextFreeId() {
  const used = new Set((allPoints.value.length ? allPoints.value : points.value).map((p) => p.id))
  let i = 1
  while (used.has(i)) i++
  return i
}
function onCreate() {
  form.value = { id: nextFreeId(), name: '', type: 'int32', access: 3 }
  createVisible.value = true
}
async function doCreate() {
  try {
    await api.createPoint({
      id: form.value.id,
      name: form.value.name,
      type: form.value.type,
      access: form.value.access
    })
    ElMessage.success(`已创建点位 ${form.value.name}`)
    createVisible.value = false
    await refresh()
  } catch (e) {
    ElMessage.error(e.message || String(e))
  }
}
async function onRename(p) {
  try {
    const { value } = await ElMessageBox.prompt('输入新名称', '重命名点位', {
      inputValue: p.name,
      inputValidator: (v) => (v && v.trim() ? true : '名称不能为空')
    })
    await api.renamePoint(p.id, value)
    ElMessage.success('已重命名')
    await refresh()
  } catch (e) {
    if (e && e.message) ElMessage.error(e.message)
  }
}
async function onDelete(p) {
  try {
    await ElMessageBox.confirm(`确认删除点位「${p.name}」(id=${p.id})？`, '删除点位', { type: 'warning' })
    await api.deletePoint(p.id)
    ElMessage.success('已删除')
    await refresh()
  } catch (e) {
    if (e && e.message) ElMessage.error(e.message)
  }
}

async function refresh() {
  try {
    if (view.value === 'partitions') {
      await loadPartitions()
    }
    await loadPage()
  } catch (e) {
    /* ignore */
  }
}

async function loadValues(list) {
  const arr = list || points.value
  await Promise.all(
    arr.map(async (p) => {
      try {
        const d = await api.getPoint(p.id)
        values[p.id] = {
          value: d.value,
          quality: d.quality === null || d.quality === undefined ? null : Number(d.quality),
          ts: d.ts
        }
      } catch (e) {
        /* ignore */
      }
    })
  )
}

function onMessage(msg) {
  if (msg.op === 'list') {
    allPoints.value = msg.points || []
    // 服务端已连接/重连；只刷新当前页并重发当前页订阅，避免 2 万点全量推送。
    loadValues(points.value)
    syncSubs()
  } else if (msg.op === 'update') {
    const id = msg.id
    // 只接收当前页订阅点的推送；其它页即使服务端误发也不污染状态。
    if (!points.value.some((p) => p.id === id)) return
    // v3.10 NOTIFY 携带 quality；null 时保留最近一次 REST 读值，兼容旧服务端。
    const prevQ = values[id] ? values[id].quality : null
    const q = msg.quality === null || msg.quality === undefined ? prevQ : Number(msg.quality)
    values[id] = { value: msg.value, quality: q, ts: msg.ts }
    if (selected.value && selected.value.id === id && selected.value.type !== 3) {
      history.value.push({ ts: Date.now(), value: Number(msg.value) })
      if (history.value.length > 300) history.value.shift()
    }
  } else if (msg.type === 'legacy') {
    legacyMode.value = true
    legacyPageSize.value = Number(msg.page_size) || 32
    if (pageSize.value !== legacyPageSize.value) {
      pageSize.value = legacyPageSize.value
      page.value = 1
      loadPage()
    }
  } else if (msg.type === 'resync') {
    // notify_drop 后去抖重发当前页订阅，避免风暴。
    clearTimeout(resyncTimer)
    resyncTimer = setTimeout(syncSubs, 2000)
  }
}

function openPoint(p) {
  selected.value = p
  drawerVisible.value = true
  meta.value = null
  const cur = values[p.id]
  history.value = cur && p.type !== 3 ? [{ ts: Date.now(), value: Number(cur.value) }] : []
  api.getMeta(p.id).then((m) => (meta.value = m)).catch(() => (meta.value = null))
}

async function onSet({ id, value }) {
  try {
    await api.setPoint(id, value)
  } catch (e) {
    // 错误状态码恢复 4xx 后必须显式提示，不能静默当成功。
    ElMessage.error(e?.message || '写入失败')
  }
}

function toggleDark(v) {
  document.documentElement.classList.toggle('dark', v)
}

onMounted(() => {
  socket = new RtdbSocket({ onMessage, onStatus: (s) => (status.value = s) })
  loadPage()
})
onBeforeUnmount(() => {
  clearTimeout(resyncTimer)
  socket && socket.close()
})
</script>

<style scoped>
.root {
  height: 100%;
}
.aside {
  background: var(--el-bg-color-overlay);
  border-right: 1px solid var(--el-border-color);
}
.logo {
  height: 56px;
  line-height: 56px;
  text-align: center;
  font-weight: 700;
  font-size: 18px;
  letter-spacing: 1px;
  border-bottom: 1px solid var(--el-border-color);
}
.header {
  display: flex;
  align-items: center;
  border-bottom: 1px solid var(--el-border-color);
  background: var(--el-bg-color);
}
.title {
  font-weight: 600;
  font-size: 16px;
}
.spacer {
  flex: 1;
}
.conn {
  margin-right: 16px;
  font-size: 13px;
  color: var(--el-text-color-secondary);
}
.main {
  padding: 16px;
  height: calc(100% - 56px);
  box-sizing: border-box;
}
.placeholder {
  height: 100%;
  display: flex;
  align-items: center;
  justify-content: center;
}
.partition-view {
  height: 100%;
}
.filter-bar {
  margin-bottom: 12px;
}
.count {
  color: var(--el-text-color-secondary);
  font-size: 13px;
}
.legacy-alert {
  margin-bottom: 12px;
}
.field-hint {
  width: 100%;
  margin-top: 2px;
  color: var(--el-text-color-secondary);
  font-size: 12px;
  line-height: 1.5;
}
</style>
