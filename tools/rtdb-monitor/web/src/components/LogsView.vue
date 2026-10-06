<template>
  <div class="logs">
    <div class="bar">
      <el-button size="small" :icon="Refresh" @click="load">刷新</el-button>
      <el-switch v-model="auto" active-text="自动刷新(3s)" />
      <el-tag size="small" type="info">共 {{ rows.length }} 条</el-tag>
      <span class="hint">服务端环形缓冲最多保留 128 条运行日志（最旧→最新）</span>
    </div>
    <el-table :data="rows" stripe border height="100%" size="small">
      <el-table-column label="时间" width="150">
        <template #default="{ row }">
          <span class="mono">{{ fmtTime(row.ts) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="级别" width="90">
        <template #default="{ row }">
          <el-tag size="small" :type="levelType(row.level)">{{ row.levelName }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="msg" label="消息" min-width="300" show-overflow-tooltip />
    </el-table>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue'
import { Refresh } from '@element-plus/icons-vue'
import { api } from '../api.js'

const rows = ref([])
const auto = ref(false)
let timer = null

function fmtTime(ns) {
  if (!ns) return '—'
  const d = new Date(Number(ns) / 1e6)
  return d.toLocaleTimeString(undefined, { hour12: false }) + '.' +
    String(d.getMilliseconds()).padStart(3, '0')
}
function levelType(l) {
  if (l === 2) return 'danger'
  if (l === 1) return 'warning'
  return 'success'
}

async function load() {
  try {
    rows.value = await api.getLogs(0)
  } catch (e) {
    rows.value = []
  }
}

function startTimer() {
  stopTimer()
  timer = setInterval(load, 3000)
}
function stopTimer() {
  if (timer) {
    clearInterval(timer)
    timer = null
  }
}

watch(auto, (v) => (v ? startTimer() : stopTimer()))
onMounted(load)
onBeforeUnmount(stopTimer)
</script>

<style scoped>
.logs {
  display: flex;
  flex-direction: column;
  height: 100%;
  gap: 10px;
}
.bar {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
}
.hint {
  margin-left: auto;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}
</style>
