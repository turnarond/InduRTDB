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
        <PointTable
          v-if="view === 'points'"
          :points="points"
          :values="values"
          @row-click="openPoint"
          @create="onCreate"
          @rename="onRename"
          @delete="onDelete"
        />
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
          <el-input-number v-model="form.id" :min="0" :max="4294967295" />
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
const points = ref([])
const values = reactive({}) // id -> {value, quality, ts}
const selected = ref(null)
const drawerVisible = ref(false)
const history = ref([])
const meta = ref(null)
const isDark = ref(false)

let socket = null

const placeholderMap = {
  partitions: ['点位分区', '类似 Redis db0/db1 的命名空间分区（需后端支持，v3.7 规划）']
}
const placeholderTitle = computed(() => (placeholderMap[view.value] ? placeholderMap[view.value][0] : ''))
const placeholderHint = computed(() => (placeholderMap[view.value] ? placeholderMap[view.value][1] : ''))

function onMenu(index) {
  view.value = index
}

// ---- v3.6 点位 CRUD ----
const createVisible = ref(false)
const form = ref({ id: 0, name: '', type: 'int32', access: 3 })
const typeNames = ['bool', 'int32', 'double', 'string', 'int64', 'uint32', 'float']

function nextFreeId() {
  const used = new Set(points.value.map((p) => p.id))
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
    points.value = await api.listPoints()
    await loadValues()
  } catch (e) {
    /* ignore */
  }
}

async function loadValues() {
  await Promise.all(
    points.value.map(async (p) => {
      try {
        const d = await api.getPoint(p.id)
        values[p.id] = { value: d.value, quality: d.quality, ts: d.ts }
      } catch (e) {
        /* ignore */
      }
    })
  )
}

function onMessage(msg) {
  if (msg.op === 'list') {
    points.value = msg.points || []
    loadValues()
  } else if (msg.op === 'update') {
    const id = msg.id
    values[id] = { value: msg.value, quality: msg.quality, ts: msg.ts }
    if (selected.value && selected.value.id === id && selected.value.type !== 3) {
      history.value.push({ ts: Date.now(), value: Number(msg.value) })
      if (history.value.length > 300) history.value.shift()
    }
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
    // 错误由后续的 WS update 失败静默；简单提示
  }
}

function toggleDark(v) {
  document.documentElement.classList.toggle('dark', v)
}

onMounted(() => {
  socket = new RtdbSocket({ onMessage, onStatus: (s) => (status.value = s) })
})
onBeforeUnmount(() => socket && socket.close())
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
</style>
