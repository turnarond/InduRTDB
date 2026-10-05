<template>
  <div>
    <el-form :inline="true" class="filter-bar">
      <el-form-item label="名称">
        <el-input v-model="name" placeholder="按名称搜索" clearable style="width: 200px" />
      </el-form-item>
      <el-form-item label="类型">
        <el-select v-model="typeFilter" placeholder="全部" clearable style="width: 130px">
          <el-option v-for="t in typeOptions" :key="t" :label="t" :value="t" />
        </el-select>
      </el-form-item>
      <el-form-item label="权限">
        <el-select v-model="accessFilter" placeholder="全部" clearable style="width: 120px">
          <el-option label="只读" :value="0" />
          <el-option label="读写" :value="1" />
        </el-select>
      </el-form-item>
      <el-form-item>
        <span class="count">共 {{ filtered.length }} 个点位</span>
      </el-form-item>
      <el-form-item>
        <el-button type="primary" :icon="Plus" @click="$emit('create')">新建点位</el-button>
      </el-form-item>
    </el-form>

    <el-table
      :data="paged"
      stripe
      border
      height="100%"
      highlight-current-row
      @row-click="(row) => $emit('row-click', row)"
    >
      <el-table-column prop="id" label="ID" width="90" />
      <el-table-column prop="name" label="名称" min-width="180" show-overflow-tooltip />
      <el-table-column label="类型" width="110">
        <template #default="{ row }">
          <el-tag size="small">{{ row.typeName }}</el-tag>
        </template>
      </el-table-column>
      <el-table-column label="权限" width="90">
        <template #default="{ row }">
          <el-tag size="small" :type="row.access === 0 ? 'success' : 'warning'">
            {{ row.access === 0 ? '只读' : '读写' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="当前值" min-width="120" show-overflow-tooltip>
        <template #default="{ row }">
          <span class="mono">{{ liveOf(row.id) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="质量" width="100">
        <template #default="{ row }">
          <el-tag size="small" :type="qualityOf(row.id) === 0 ? 'success' : 'info'">
            {{ qualityOf(row.id) === 0 ? 'Good' : '—' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="操作" width="150" fixed="right">
        <template #default="{ row }">
          <el-button size="small" @click.stop="$emit('rename', row)">重命名</el-button>
          <el-button size="small" type="danger" @click.stop="$emit('delete', row)">删除</el-button>
        </template>
      </el-table-column>
    </el-table>

    <div class="pager">
      <el-pagination
        v-model:current-page="page"
        v-model:page-size="pageSize"
        :total="filtered.length"
        :page-sizes="[10, 20, 50, 100]"
        layout="total, sizes, prev, pager, next, jumper"
        background
      />
    </div>
  </div>
</template>

<script setup>
import { ref, computed } from 'vue'
import { Plus } from '@element-plus/icons-vue'

const props = defineProps({
  points: { type: Array, default: () => [] },
  values: { type: Object, default: () => ({}) } // id -> {value,quality,ts}
})
const emit = defineEmits(['row-click', 'create', 'rename', 'delete'])

const name = ref('')
const typeFilter = ref('')
const accessFilter = ref('')
const page = ref(1)
const pageSize = ref(20)

const typeOptions = computed(() => [...new Set(props.points.map((p) => p.typeName))])

const filtered = computed(() =>
  props.points.filter((p) => {
    if (name.value && !p.name.toLowerCase().includes(name.value.toLowerCase())) return false
    if (typeFilter.value && p.typeName !== typeFilter.value) return false
    if (accessFilter.value !== '' && p.access !== accessFilter.value) return false
    return true
  })
)
const paged = computed(() => {
  const start = (page.value - 1) * pageSize.value
  return filtered.value.slice(start, start + pageSize.value)
})

function liveOf(id) {
  const v = props.values[id]
  return v ? String(v.value) : '—'
}
function qualityOf(id) {
  const v = props.values[id]
  return v ? v.quality : null
}
</script>

<style scoped>
.filter-bar {
  margin-bottom: 12px;
}
.count {
  color: var(--el-text-color-secondary);
  font-size: 13px;
}
.pager {
  margin-top: 12px;
  display: flex;
  justify-content: flex-end;
}
</style>
