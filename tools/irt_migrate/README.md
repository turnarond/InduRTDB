# irt_migrate — InduRTDB v1/v2 → 当前版（v3）离线迁移工具

纯 Python 实现的共享内存段离线迁移工具，配套 **v3.9 段版本 3**。
不依赖 C 库，逐字节复刻 v1 / v2 / v3 段布局（与 `src/internal/irt_types.h`、
`src/core/irt_shm.h`、`src/core/irt_index.c` 严格一致）。

> 适用平台：**Linux**（`/dev/shm`）。SylixOS 路径不同，离线迁移通常
> 在构建/运维主机（Linux）上完成，目标机直接拉取已迁移的段或重建。

## 背景

段格式版本演进：

| 段版本 | 起始版本 | 变化 |
|---|---|---|
| 1 | v3.3 及以前 | Header 64B，无索引/元数据区 |
| 2 | v3.4 | Header 64B → 128B，新增索引区 / 元数据区 |
| 3 | v3.9 | 点位 `reserved[0:4)` 更名 `partition`；name→id 索引键含分区 |

**旧段被新库一律拒绝挂载**（`IRT_SHM_ERR_VERSION`），需停机迁移或重建，
**不支持新旧进程混跑**。本工具负责把旧 v1 / v2 段的数据搬到当前版（v3）段。

> v2 / v3 布局**逐字节相同**，仅 Header `version` 字段值与点位 120..123
> 的语义不同，故 v2→v3 迁移在字节层面几乎无搬运成本（索引仍须重建，
> 因为索引键从 `name` 变为 `(partition, name)`）。

## 迁移范围（方案 §6）

| 项 | 处理 |
|---|---|
| 点位值 / 类型 / 质量 / `timestamp_ns`（入库时刻） | 逐字节搬运（v1/v2/v3 点位同为 128B，偏移不变） |
| 点位名 `name` | 搬运，并在目标段内**重建 name→id 索引**（键含分区） |
| 点位 `partition`（v3.9 新增语义） | v1/v2 源段无此语义 → 全部落**默认分区 0**（120..123 显式清零） |
| `source_timestamp_ns`（采集时刻） | 默认**置 0**（`--source-ts-mode keep` 可保留） |
| 元数据区（v2 新增，32B/点） | **v2 源段原样保留**；v1 无元数据区故清零 |
| 订阅者心跳区 | 清零（离线迁移，由新 `rtdbd` 重建） |

## 用法

```bash
# 迁移 v1/v2 段 src_id -> 当前版（v3）段 dst_id（源段保留，dst 已存在需 --force）
python3 -m irt_migrate migrate --src <src_id> --dst <dst_id> [--source-ts-mode zero|keep] [--force]

# 独立校验迁移前后一致性（不依赖迁移写入逻辑）
python3 -m irt_migrate validate --src <src_id> --dst <dst_id> [--sample N]

# 查看某段的版本与布局（v1 / v2 / v3 均可）
python3 -m irt_migrate info <id>
```

- 段名规则：`/dev/shm/indurtdb_<id>`。
- 迁移成功后自动执行 `validate`；任一不一致即非零退出。
- **版本保护**：源段须为 **v1 或 v2**；v3（已是目标格式）与任何未知版本
  一律拒绝（`ValueError`），避免误覆盖或重复迁移。

## 工程结构

```
irt_migrate/
  pyproject.toml
  README.md
  irt_migrate/
    __init__.py       # 包导出（仅暴露子模块，避免与同名函数冲突）
    layout.py         # 布局常量 + Header 解析/CRC + FNV-1a + 索引重建（复刻 C 实现）
    migrate.py        # read_v1 / read_v2_source / read_source / build_target / migrate
    validate.py       # 独立校验 + read_v2_segment（读 v3 目标段）
    cli.py            # argparse 入口
    __main__.py
  tests/
    test_migrate.py   # 单元/往返自检（可直接 `python3 tests/test_migrate.py`）
```

> 说明：`build_v2_header` / `build_v2_segment` / `read_v2_segment` 等函数名
> 沿用历史命名，自 v3.9 起实际输出/解析的是**当前版段**（版本常量
> `SHM_VERSION_CURRENT`）。布局结构未变，仅版本值前进，故未做函数改名以
> 避免大面积 churn；新增的 v2 **源段**读取器命名为
> `read_v2_source_segment`，与读 v3 目标段的 `read_v2_segment` 区分。

## 自检

```bash
cd tools/irt_migrate
python3 tests/test_migrate.py
```

## 升级与回退流程（运维）

1. **先备份段（关键）**：在 rtdbd **仍运行时** 执行 `cp /dev/shm/indurtdb_<id> <backup>`。因为 rtdbd 优雅退出（SIGTERM）会调用 `indurtdb_h_close()`，作为 owner 执行 `shm_unlink`，**段会在停机瞬间被删除**；先停机再迁移将无段可迁。
2. **停机**：停止所有使用旧段的进程（v3.3.x / v3.4–v3.8 / node-server）。正常 SIGTERM 停机即删除原段（见上）；应急可用 `kill -9`（不执行 `shm_unlink`，段得以保留，但会遗留 seqlock 写锁，**仅应急**）。
3. **恢复 + 迁移**：先把快照恢复到 `/dev/shm/indurtdb_<id>`，再执行 `python3 -m irt_migrate migrate --src <id> --dst <id> --force`
   （同 id 原地迁移：先完整读入源段内容到内存，再以 `O_CREAT|O_RDWR` +
   `ftruncate` + `mmap` **原地覆写**同一路径；**不会 `shm_unlink`**，
   因此 inode/路径保持不变）；或 `--dst <new_id>` 保留旧源段作为备份。
4. **校验**：工具自动校验；可再跑 `validate` 复核。
5. **全进程同时升级**到 v3.9，启动 `rtdbd` 重建心跳。
6. **回退**：若需回退，重新部署旧版本并清理 `/dev/shm/indurtdb_*`
   （新版段对旧库不可读，会被版本协商拒绝）；若保留了 v1/v2 备份段可直接复用。

## 关键不变式

- **零堆分配 / 运行期布局**：工具不引入额外内存模型，仅读写共享内存字节。
- **CRC 一致**：段 Header 的 CRC32 覆盖 `[0:16] + [48:72]`，与
  `irt_header_crc32_of`（标准 CRC-32/IEEE 802.3）逐字节一致；
  迁移后段能被当前库 `indurtdb_h_open` 成功挂载即证明其字节级兼容。
- **索引一致**：FNV-1a 32 + 线性探测 + 墓碑，与 `irt_index.c` 一致，
  `find_by_name` 在迁移后段中可用。
- **迁移对源段非破坏、目标原地写入**：迁移只读源段，不改源段在盘字节；目标以 `O_CREAT|O_RDWR` + `ftruncate` + `mmap` 原地写入、**不 `shm_unlink`**，故 `--dst <other_id>` 可完整保留源段用于回退。演练证据见 [`docs/08-验证记录/v3.9-T8-迁移与回退演练.md`](../../docs/08-验证记录/v3.9-T8-迁移与回退演练.md)。
