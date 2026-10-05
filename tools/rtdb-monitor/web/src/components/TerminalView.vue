<template>
  <div class="term">
    <div class="bar">
      <el-button size="small" @click="run('help')">help</el-button>
      <el-button size="small" @click="run('ping')">ping</el-button>
      <el-button size="small" @click="run('list')">list</el-button>
      <el-button size="small" @click="clear">清屏</el-button>
      <span class="hint">支持：list / get / set / find / meta / create / del / rename（参数可用点位名）</span>
    </div>
    <div ref="box" class="out">
      <div v-for="(l, i) in lines" :key="i" :class="lineCls(l)">{{ l }}</div>
    </div>
    <div class="in">
      <span class="prompt">&gt;</span>
      <el-input
        v-model="cmd"
        placeholder="输入命令后回车，如：create 20 Pump_02.Status int32"
        @keyup.enter="run(cmd)"
        @keyup.up="histUp"
        @keyup.down="histDown"
      />
      <el-button type="primary" :icon="Position" @click="run(cmd)">执行</el-button>
    </div>
  </div>
</template>

<script setup>
import { ref, nextTick } from 'vue'
import { Position } from '@element-plus/icons-vue'
import { api } from '../api.js'

const emit = defineEmits(['changed'])

const cmd = ref('')
const lines = ref(['rtdb-monitor 终端 · 输入 help 查看命令'])
const box = ref(null)
const history = ref([])
const hidx = ref(-1)

function push(ls) {
  lines.value.push(...ls)
  nextTick(() => {
    if (box.value) box.value.scrollTop = box.value.scrollHeight
  })
}
function lineCls(l) {
  if (l.startsWith('ERR')) return 'l-err'
  if (l.startsWith('>')) return 'l-cmd'
  return 'l-out'
}
function clear() {
  lines.value = []
}

async function run(c) {
  const line = (c || '').trim()
  if (!line) return
  cmd.value = ''
  history.value.push(line)
  hidx.value = history.value.length
  push([`> ${line}`])
  try {
    const r = await api.runCmd(line)
    push(r.output || [])
    if (/^(create|del|delete|rename)\b/.test(line)) emit('changed')
  } catch (e) {
    push([`ERR: ${e.message || e}`])
  }
}
function histUp() {
  if (!history.value.length) return
  hidx.value = Math.max(0, hidx.value - 1)
  cmd.value = history.value[hidx.value] || ''
}
function histDown() {
  if (!history.value.length) return
  hidx.value = Math.min(history.value.length - 1, hidx.value + 1)
  cmd.value = history.value[hidx.value] ?? ''
}
</script>

<style scoped>
.term {
  display: flex;
  flex-direction: column;
  height: 100%;
  gap: 10px;
}
.bar {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}
.hint {
  margin-left: auto;
  font-size: 12px;
  color: var(--el-text-color-secondary);
}
.out {
  flex: 1;
  overflow: auto;
  background: var(--el-fill-color-darker);
  border: 1px solid var(--el-border-color);
  border-radius: 6px;
  padding: 12px;
  font-family: 'JetBrains Mono', 'SFMono-Regular', Consolas, Menlo, monospace;
  font-size: 13px;
  line-height: 1.6;
  white-space: pre-wrap;
}
.in {
  display: flex;
  align-items: center;
  gap: 8px;
}
.prompt {
  font-family: monospace;
  font-size: 16px;
  color: var(--el-color-primary);
}
.l-cmd {
  color: var(--el-color-primary);
}
.l-err {
  color: var(--el-color-danger);
}
.l-out {
  color: var(--el-text-color-regular);
}
</style>
