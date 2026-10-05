<template>
  <el-drawer
    :model-value="modelValue"
    :title="point ? point.name : '点位详情'"
    size="460px"
    @update:model-value="$emit('update:modelValue', $event)"
  >
    <template v-if="point">
      <el-descriptions :column="1" border size="small">
        <el-descriptions-item label="ID">{{ point.id }}</el-descriptions-item>
        <el-descriptions-item label="名称">{{ point.name }}</el-descriptions-item>
        <el-descriptions-item label="类型">
          <el-tag size="small">{{ point.typeName }}</el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="访问权限">
          <el-tag size="small" :type="point.access === 0 ? 'success' : 'warning'">
            {{ point.access === 0 ? '只读' : '读写' }}
          </el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="当前值">
          <span class="mono">{{ displayValue }}</span>
        </el-descriptions-item>
        <el-descriptions-item label="质量">
          <el-tag size="small" :type="qualityType">{{ qualityText }}</el-tag>
        </el-descriptions-item>
        <el-descriptions-item label="时间戳">
          <span class="mono">{{ live && live.ts ? formatTs(live.ts) : '—' }}</span>
        </el-descriptions-item>
      </el-descriptions>

      <el-divider>实时曲线</el-divider>
      <RealtimeChart :history="history" :numeric="numeric" />

      <el-divider>设置点位值</el-divider>
      <div style="display: flex; gap: 8px">
        <el-input
          v-if="numeric"
          v-model="setVal"
          type="number"
          placeholder="输入数值"
          style="flex: 1"
        />
        <el-input
          v-else
          v-model="setVal"
          placeholder="输入字符串"
          style="flex: 1"
        />
        <el-button type="primary" :icon="Upload" @click="onSet">写入</el-button>
      </div>

      <el-divider>元数据</el-divider>
      <el-descriptions v-if="meta" :column="1" border size="small">
        <el-descriptions-item label="范围">
          {{ fmt(meta.eurMin) }} ~ {{ fmt(meta.eurMax) }}
        </el-descriptions-item>
        <el-descriptions-item label="单位">{{ meta.unit || '—' }}</el-descriptions-item>
        <el-descriptions-item label="描述">{{ meta.desc || '—' }}</el-descriptions-item>
      </el-descriptions>
      <el-empty v-else description="无元数据" :image-size="60" />
    </template>
  </el-drawer>
</template>

<script setup>
import { ref, computed, watch } from 'vue'
import { Upload } from '@element-plus/icons-vue'
import RealtimeChart from './RealtimeChart.vue'

const props = defineProps({
  modelValue: Boolean,
  point: Object,
  live: Object,
  history: { type: Array, default: () => [] },
  meta: Object
})
const emit = defineEmits(['update:modelValue', 'set'])

const setVal = ref('')
const numeric = computed(() => props.point && props.point.type !== 3)
const displayValue = computed(() => (props.live ? String(props.live.value) : '—'))

const qualityText = computed(() => (props.live && props.live.quality === 0 ? 'Good' : 'Bad/Unknown'))
const qualityType = computed(() => (props.live && props.live.quality === 0 ? 'success' : 'danger'))

watch(
  () => props.point,
  (p) => {
    setVal.value = p && props.live ? String(props.live.value) : ''
  }
)

function onSet() {
  if (setVal.value === '' || setVal.value === null) return
  const v = numeric.value ? Number(setVal.value) : setVal.value
  emit('set', { id: props.point.id, value: v })
}
function fmt(v) {
  return v === undefined || v === null ? '—' : v
}
function formatTs(ts) {
  const d = new Date(Number(ts) * (ts < 1e12 ? 1000 : 1))
  return d.toLocaleString()
}
</script>
