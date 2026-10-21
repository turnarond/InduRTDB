<template>
  <div>
    <el-form :inline="true" class="filter-bar">
      <el-form-item label="名称">
        <el-input
          v-model="name"
          placeholder="按名称搜索"
          clearable
          style="width: 200px"
        />
      </el-form-item>
      <el-form-item label="类型">
        <el-select v-model="typeFilter" placeholder="全部" clearable style="width: 130px">
          <el-option v-for="t in typeOptions" :key="t" :label="t" :value="t" />
        </el-select>
      </el-form-item>
      <el-form-item label="权限">
        <el-select v-model="accessFilter" placeholder="全部" clearable style="width: 120px">
          <el-option label="只读" :value="1" />
          <el-option label="读写" :value="3" />
        </el-select>
      </el-form-item>
      <el-form-item>
        <span class="count">共 {{ total }} 个点位（当前页 {{ points.length }}）</span>
      </el-form-item>
      <el-form-item>
        <el-button type="primary" :icon="Plus" @click="$emit('create')">新建点位</el-button>
      </el-form-item>
    </el-form>

    <el-table
      :data="points"
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
      <el-table-column prop="partition" label="分区" width="90" />
      <el-table-column label="权限" width="90">
        <template #default="{ row }">
          <el-tag size="small" :type="row.access === 1 ? 'success' : 'warning'">
            {{ row.access === 1 ? '只读' : '读写' }}
          </el-tag>
        </template>
      </el-table-column>
      <el-table-column label="当前值" min-width="120" show-overflow-tooltip>
        <template #default="{ row }">
          <span class="mono">{{ liveOf(row.id) }}</span>
        </template>
      </el-table-column>
      <el-table-column label="质量" width="130">
        <template #default="{ row }">
          <el-tag size="small" :type="qualityTagType(values[row.id]?.quality)">
            {{ qualityLabel(values[row.id]?.quality) }}
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
        :current-page="page"
        :page-size="pageSize"
        :page-sizes="pageSizes"
        :total="total"
        layout="total, sizes, prev, pager, next, jumper"
        background
        @current-change="$emit('page-change', $event)"
        @size-change="$emit('page-size-change', $event)"
      />
    </div>
  </div>
</template>

<script setup>
import { ref, watch, onBeforeUnmount } from 'vue'
import { Plus } from '@element-plus/icons-vue'
import { qualityLabel, qualityTagType } from '../quality.js'

const props = defineProps({
  points: { type: Array, default: () => [] }, // 当前页
  values: { type: Object, default: () => ({}) }, // id -> {value,quality,ts}
  total: { type: Number, default: 0 },
  page: { type: Number, default: 1 },
  pageSize: { type: Number, default: 50 },
  pageSizes: { type: Array, default: () => [10, 20, 32, 50, 100] },
  typeOptions: { type: Array, default: () => [] }
})
const emit = defineEmits([
  'row-click', 'create', 'rename', 'delete',
  'page-change', 'page-size-change', 'filter'
])

const name = ref('')
const typeFilter = ref('')
const accessFilter = ref('')
let filterTimer = null

watch([name, typeFilter, accessFilter], () => {
  clearTimeout(filterTimer)
  filterTimer = setTimeout(() => {
    emit('filter', {
      q: name.value.trim(),
      type: typeFilter.value || '',
      access: accessFilter.value === '' ? null : accessFilter.value
    })
  }, 250)
})
onBeforeUnmount(() => clearTimeout(filterTimer))

function liveOf(id) {
  const v = props.values[id]
  return v ? String(v.value) : '—'
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
