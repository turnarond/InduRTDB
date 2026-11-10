<template>
  <div class="peers">
    <div class="bar">
      <el-button size="small" :icon="Refresh" @click="load">刷新</el-button>
      <el-switch v-model="auto" active-text="自动刷新(3s)" />
      <el-tag size="small" type="info">共 {{ rows.length }} 个成员</el-tag>
      <span class="hint">{{ notes }}</span>
      <span class="hint">--sync-self-addr 指定本节点地址，含该地址的 SET_PEERS 会被拒绝</span>
      <span class="hint">VSOA /sync/peers/{set,get} 限 sync_admin + loopback（--vsoa-token-file）</span>
      <el-tag size="small" type="success">UDS 已连接</el-tag>
      <el-tag size="small" :type="channels.vsoa_enabled ? 'success' : 'info'">
        VSOA {{ channels.vsoa_enabled ? '127.0.0.1:' + channels.vsoa_port : '未启用' }}
      </el-tag>
    </div>
    <el-alert
      type="info"
      :closable="false"
      show-icon
      title="预留字段提示"
      description="boot_id / applied_offset / lag 为协议预留字段，当前恒为 0（IR1 全量补齐与 IR6 重启语义尚未实现），不代表真实同步进度。"
      class="reserved-hint"
    />
    <el-table :data="rows" stripe border height="100%" size="small">
      <el-table-column prop="addr" label="地址" width="160" />
      <el-table-column prop="node_id" label="节点 ID" width="150" show-overflow-tooltip />
      <el-table-column label="状态" width="110">
        <template #default="{ row }">
          <el-tag size="small" :type="stateType(row.state)">{{ stateName(row.state) }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column prop="boot_id" label="boot_id（预留）" width="150" />
      <el-table-column prop="applied_offset" label="applied_offset（预留）" width="190" />
      <el-table-column prop="lag" label="lag（预留）" width="140" />
    </el-table>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue'
import { Refresh } from '@element-plus/icons-vue'
import { api } from '../api.js'

const rows = ref([])
const notes = ref('成员表来自 rtdbd 进程内定长表（最多 32 条）；整组替换请用命令终端 peers set。')
const auto = ref(false)
const channels = ref({ uds: true, vsoa_enabled: false, vsoa_port: 0 })
let timer = null

const STATE_NAMES = ['unknown', 'joining', 'active', 'degraded', 'lost']
function stateName(s) {
  return STATE_NAMES[s] || `state=${s}`
}
function stateType(s) {
  if (s === 2) return 'success'
  if (s === 1) return 'warning'
  if (s === 3 || s === 4) return 'danger'
  return 'info'
}

async function load() {
  try {
    const data = await api.getPeers()
    rows.value = Array.isArray(data) ? data : (data.peers || [])
    if (data && data.notes) notes.value = data.notes
    if (data && data.channels) channels.value = data.channels
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

watch(auto, (on) => (on ? startTimer() : stopTimer()))
onMounted(load)
onBeforeUnmount(stopTimer)
</script>

<style scoped>
.peers { height: 100%; display: flex; flex-direction: column; gap: 8px; }
.bar { display: flex; align-items: center; gap: 12px; }
.hint { color: var(--el-text-color-secondary); font-size: 12px; }
.reserved-hint { margin-bottom: 4px; }
</style>
