"""irt_migrate 命令行入口。

用法:
  python -m irt_migrate migrate  --src <v1/v2_id> --dst <v3_id> [--source-ts-mode zero|keep] [--force]
  python -m irt_migrate validate --src <v1/v2_id> --dst <v3_id> [--sample N]
  python -m irt_migrate info     <id>
"""
from __future__ import annotations

import argparse
import json
import sys

from . import layout as L
from . import migrate as M
from . import validate as V


def _cmd_migrate(args) -> int:
    try:
        stat = M.migrate(args.src, args.dst,
                         source_ts_mode=args.source_ts_mode, force=args.force)
    except FileExistsError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 2
    except ValueError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 2
    print(json.dumps({"op": "migrate", **stat}, ensure_ascii=False, indent=2))
    # 迁移后顺带校验
    try:
        rep = V.validate(args.src, args.dst,
                         source_ts_mode=args.source_ts_mode,
                         sample=args.sample)
        print(json.dumps({"op": "validate", **rep}, ensure_ascii=False,
                         indent=2))
    except ValueError as e:
        print(f"校验失败: {e}", file=sys.stderr)
        return 1
    return 0


def _cmd_validate(args) -> int:
    try:
        rep = V.validate(args.src, args.dst,
                         source_ts_mode=args.source_ts_mode,
                         sample=args.sample)
    except ValueError as e:
        print(f"校验失败: {e}", file=sys.stderr)
        return 1
    print(json.dumps({"op": "validate", **rep}, ensure_ascii=False, indent=2))
    return 0


def _cmd_info(args) -> int:
    path = L.shm_path(args.id)
    import os
    if not os.path.exists(path):
        print(f"段不存在: {path}", file=sys.stderr)
        return 1
    try:
        version = M.peek_segment_version(args.id)
        if version == L.SHM_VERSION_V1:
            seg = M.read_v1_segment(args.id)
            kind = "v1"
            info = {
                "id": args.id,
                "version": seg["version"],
                "max_points": seg["max_points"],
                "max_subscribers": seg["max_subscribers"],
                "total_bytes": L.total_size_v1(seg["max_points"],
                                               seg["max_subscribers"]),
            }
        elif version == L.SHM_VERSION_V2:
            seg = M.read_v2_source_segment(args.id)
            kind = "v2"
            info = {
                "id": args.id,
                "version": seg["version"],
                "max_points": seg["max_points"],
                "max_subscribers": seg["max_subscribers"],
                "index_capacity": L.layout_buckets(seg["max_points"]),
                "index_count": seg["index_count"],
                "meta_bytes": L.layout_meta_size(seg["max_points"]),
                "total_bytes": seg["total"],
                "crc32": f"{seg['crc32']:#010x}",
            }
        elif version == L.SHM_VERSION_V3:
            seg = V.read_v2_segment(args.id)
            kind = "v3"
            info = {
                "id": args.id,
                "version": seg["version"],
                "max_points": seg["max_points"],
                "max_subscribers": seg["max_subscribers"],
                "index_capacity": L.layout_buckets(seg["max_points"]),
                "index_count": seg["index_count"],
                "meta_bytes": L.layout_meta_size(seg["max_points"]),
                "total_bytes": seg["total"],
                "crc32": f"{seg['crc32']:#010x}",
            }
        else:
            print(f"无法识别的段: 未知版本 {version}", file=sys.stderr)
            return 1
    except ValueError as e:
        print(f"无法识别的段: {e}", file=sys.stderr)
        return 1
    info["kind"] = kind
    print(json.dumps(info, ensure_ascii=False, indent=2))
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="irt_migrate",
        description="InduRTDB v1/v2 -> v3 共享内存段离线迁移工具")
    sub = p.add_subparsers(dest="cmd", required=True)

    m = sub.add_parser("migrate", help="迁移 v1/v2 段到 v3 段")
    m.add_argument("--src", required=True, help="源 v1/v2 实例 id")
    m.add_argument("--dst", required=True, help="目标 v3 实例 id")
    m.add_argument("--source-ts-mode", choices=["zero", "keep"], default="zero",
                   help="source_timestamp_ns 处理：zero=置0(默认)，keep=保留")
    m.add_argument("--force", action="store_true",
                   help="覆盖已存在的目标段")
    m.add_argument("--sample", type=int, default=0,
                   help="校验抽样数量，0=全量")
    m.set_defaults(func=_cmd_migrate)

    v = sub.add_parser("validate", help="独立校验迁移前后一致性")
    v.add_argument("--src", required=True)
    v.add_argument("--dst", required=True)
    v.add_argument("--source-ts-mode", choices=["zero", "keep"], default="zero")
    v.add_argument("--sample", type=int, default=0)
    v.set_defaults(func=_cmd_validate)

    i = sub.add_parser("info", help="查看段的版本与布局")
    i.add_argument("id")
    i.set_defaults(func=_cmd_info)
    return p


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
