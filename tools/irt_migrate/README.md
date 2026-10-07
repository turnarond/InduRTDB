# irt_migrate — InduRTDB v1 → v2 离线迁移工具

纯 Python 实现的共享内存段离线迁移工具，配套 **v3.4 布局 v2（路线乙）**。
不依赖 C 库，逐字节复刻 v1 / v2 段布局（与 `src/internal/irt_types.h`、
`src/core/irt_shm.h`、`src/core/irt_index.c` 严格一致）。

> 适用平台：**Linux**（`/dev/shm`）。SylixOS 路径不同，离线迁移通常
> 在构建/运维主机（Linux）上完成，目标机直接拉取已迁移的段或重建。

## 背景

v3.4 段格式版本由 1 升到 2（Header 64B → 128B，新增索引区 / 元数据区）。
**v1 段被 v3.4 库一律拒绝挂载**（`IRT_SHM_ERR_VERSION`），需停机迁移或重建，
**不支持新旧进程混跑**。本工具负责把旧 v1 段的数据搬到 v2 段。

## 迁移范围（方案 §6）

| 项 | 处理 |
|---|---|
| 点位值 / 类型 / 质量 / `timestamp_ns`（入库时刻） | 逐字节搬运（v1/v2 点位同为 128B，偏移不变） |
| 点位名 `name` | 搬运，并在 v2 段内**重建 name→id 索引** |
| `source_timestamp_ns`（采集时刻） | 默认**置 0**（`--source-ts-mode keep` 可保留） |
| 元数据区（v2 新增） | 清零 |
| 订阅者心跳区 | 清零（离线迁移，由新 `rtdbd` 重建） |

## 用法

```bash
# 迁移 v1 段 src_id -> v2 段 dst_id（源段保留，dst 已存在需 --force）
python3 -m irt_migrate migrate --src <src_id> --dst <dst_id> [--source-ts-mode zero|keep] [--force]

# 独立校验迁移前后一致性（不依赖迁移写入逻辑）
python3 -m irt_migrate validate --src <src_id> --dst <dst_id> [--sample N]

# 查看某段的版本与布局
python3 -m irt_migrate info <id>
```

- 段名规则：`/dev/shm/indurtdb_<id>`。
- 迁移成功后自动执行 `validate`；任一不一致即非零退出。
- **版本保护**：源段已是 v2 时拒绝迁移（避免误覆盖旧段）。

## 工程结构

```
irt_migrate/
  pyproject.toml
  README.md
  irt_migrate/
    __init__.py       # 包导出（仅暴露子模块，避免与同名函数冲突）
    layout.py         # 布局常量 + Header CRC + FNV-1a + 索引重建（复刻 C 实现）
    migrate.py        # read_v1 / build_v2 / write_segment / migrate
    validate.py       # 独立校验 + read_v2
    cli.py            # argparse 入口
    __main__.py
  tests/
    test_migrate.py   # 单元/往返自检（可直接 `python3 tests/test_migrate.py`）
```

## 自检

```bash
cd tools/irt_migrate
python3 tests/test_migrate.py
```

## 升级与回退流程（运维）

1. **停机**：停止所有使用旧段的进程（v3.3.x / node-server）。
2. **迁移**：`python3 -m irt_migrate migrate --src <id> --dst <id> --force`
   （同 id 原地迁移会先读源段、再 `shm_unlink` 旧段、以同 id 重建 v2 段）；
   或 `--dst <new_id>` 保留 v1 源段作为备份。
3. **校验**：工具自动校验；可再跑 `validate` 复核。
4. **全进程同时升级**到 v3.4，启动 `rtdbd` 重建心跳。
5. **回退**：若需回退，重新部署 v3.3.x 并清理 `/dev/shm/indurtdb_*`
   （v2 段对 v3.3 不可读，会被版本协商拒绝）；若保留了 v1 备份段可直接复用。

## 关键不变式

- **零堆分配 / 运行期布局**：工具不引入额外内存模型，仅读写共享内存字节。
- **CRC 一致**：v2 Header 的 CRC32 覆盖 `[0:16] + [48:72]`，与
  `irt_header_crc32_of`（标准 CRC-32/IEEE 802.3）逐字节一致；
  迁移后段能被 v3.4 库 `indurtdb_h_open` 成功挂载即证明其字节级兼容。
- **索引一致**：FNV-1a 32 + 线性探测 + 墓碑，与 `irt_index.c` 一致，
  `find_by_name` 在迁移后段中可用。
