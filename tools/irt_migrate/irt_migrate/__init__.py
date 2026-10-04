"""InduRTDB 离线迁移工具：v1 段 -> v2 段。

纯 Python 实现（不依赖 C 库），逐字节复刻 v1/v2 共享内存布局。
详见 README.md 与 docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md §6。

注意：子模块 migrate / validate 与本包同名函数并存，包命名空间只暴露
子模块（避免 `from irt_migrate import migrate` 取到函数而非模块）。
函数经子模块访问：migrate.migrate(...)、validate.validate(...)。
"""
from __future__ import annotations

__version__ = "1.0.0"

from . import layout
from . import migrate
from . import validate

# 便捷常量（来自 layout 子模块，不与子模块名冲突）
from .layout import (MAGIC, SHM_VERSION_V1, SHM_VERSION_V2, HEADER_V1_SIZE,
                     HEADER_V2_SIZE, POINT_SIZE, SUBS_ENTRY_SIZE,
                     INDEX_SLOT_SIZE, META_SIZE_PER_POINT, NAME_OFFSET,
                     SOURCE_TS_OFFSET, INDEX_EMPTY, INDEX_TOMBSTONE,
                     shm_path, layout_buckets, layout_index_size,
                     layout_meta_size, off_points_v2, off_index_v2,
                     off_meta_v2, off_subs_v2, total_size_v2,
                     total_size_v1, parse_v1_header, build_v2_header,
                     read_point_name, name_eq, fnv1a32, build_index,
                     index_probe)

__all__ = [
    "layout", "migrate", "validate",
    "MAGIC", "SHM_VERSION_V1", "SHM_VERSION_V2",
    "HEADER_V1_SIZE", "HEADER_V2_SIZE", "POINT_SIZE", "SUBS_ENTRY_SIZE",
    "INDEX_SLOT_SIZE", "META_SIZE_PER_POINT", "NAME_OFFSET",
    "SOURCE_TS_OFFSET", "INDEX_EMPTY", "INDEX_TOMBSTONE",
    "shm_path", "layout_buckets", "layout_index_size", "layout_meta_size",
    "off_points_v2", "off_index_v2", "off_meta_v2", "off_subs_v2",
    "total_size_v2", "total_size_v1", "parse_v1_header", "build_v2_header",
    "read_point_name", "name_eq", "fnv1a32", "build_index", "index_probe",
]
