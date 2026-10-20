#!/usr/bin/env python3
"""生成一个合成 v2 共享内存段，供 E2E 迁移场景使用。

复用 tests/test_migrate.py 中的 _build_v2_segment（避免重复布局逻辑），
随后从落盘段中回读点位名，确保打印出来的是真实 ground truth。

用法:
  python3 tests/make_v2_fixture.py --id e2e_v2src \
      --max-points 16 --max-subs 2 --names 3
"""
from __future__ import annotations

import argparse
import os
import sys

# 让脚本可直接运行：既能 import 同目录的 test_migrate，也能 import irt_migrate 包
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_migrate import _build_v2_segment  # noqa: E402

from irt_migrate import layout as L  # noqa: E402


def _read_back_names(instance_id: str, count: int) -> list[str]:
    """从已写入的 v2 段回读前 count 个点位名。"""
    path = L.shm_path(instance_id)
    with open(path, "rb") as fh:
        raw = fh.read()
    points = raw[L.off_points_v2():L.off_points_v2() + count * L.POINT_SIZE]
    return [L.read_point_name(points, pid).decode() for pid in range(count)]


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="生成合成 v2 共享内存段（E2E 迁移夹具）")
    parser.add_argument("--id", required=True, help="实例 id（段名 /dev/shm/indurtdb_<id>）")
    parser.add_argument("--max-points", type=int, default=16)
    parser.add_argument("--max-subs", type=int, default=2)
    parser.add_argument("--names", type=int, default=3,
                        help="前 N 个点位写入名字 v2_tag_<pid:03d>")
    args = parser.parse_args(argv)

    if args.max_points <= 0:
        print("错误: --max-points 必须为正整数", file=sys.stderr)
        return 2
    if args.names < 0 or args.names > args.max_points:
        print("错误: --names 须在 [0, --max-points] 区间内", file=sys.stderr)
        return 2

    try:
        _build_v2_segment(args.id, args.max_points, args.max_subs,
                          seed_names=args.names)
        names = _read_back_names(args.id, args.names)
    except Exception as exc:  # noqa: BLE001 - 夹具失败须以非零退出
        print(f"错误: 生成 v2 段失败: {exc}", file=sys.stderr)
        return 1

    print(f"v2 fixture ready: id={args.id} "
          f"path={L.shm_path(args.id)} names={names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
