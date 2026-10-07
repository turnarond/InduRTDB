<template>
  <div v-if="!numeric" class="chart-hint">字符串/布尔点位暂不支持实时曲线</div>
  <div v-else ref="el" class="chart"></div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount, watch } from 'vue'
import * as echarts from 'echarts'

const props = defineProps({
  history: { type: Array, default: () => [] }, // [{ts, value}]
  numeric: { type: Boolean, default: true }
})

const el = ref(null)
let chart = null

function render() {
  if (!chart || !props.numeric) return
  chart.setOption({
    grid: { left: 48, right: 16, top: 16, bottom: 28 },
    tooltip: { trigger: 'axis' },
    xAxis: {
      type: 'time',
      axisLabel: { formatter: (v) => new Date(v).toLocaleTimeString() }
    },
    yAxis: { type: 'value', scale: true },
    series: [
      {
        type: 'line',
        showSymbol: false,
        smooth: true,
        data: props.history.map((d) => [d.ts, d.value]),
        areaStyle: { opacity: 0.15 }
      }
    ]
  })
}

onMounted(() => {
  if (props.numeric) {
    chart = echarts.init(el.value)
    render()
    window.addEventListener('resize', resize)
  }
})
onBeforeUnmount(() => {
  window.removeEventListener('resize', resize)
  chart && chart.dispose()
})
function resize() {
  chart && chart.resize()
}
watch(() => props.history, render, { deep: true })
</script>

<style scoped>
.chart {
  width: 100%;
  height: 240px;
}
.chart-hint {
  color: var(--el-text-color-secondary);
  font-size: 13px;
  padding: 24px 0;
  text-align: center;
}
</style>
