"""irt-diag 命令行入口：inspect / smoke / leak。

统一参数、退出码与日志，便于接入 CI：
  inspect  -> 0 健康 / 1 不健康 / 2 段不存在或解析失败
  smoke    -> harness 退出码（0 表示各步骤成功）
  leak     -> 0 无泄漏 / 3 检测到 fd 增长 / 2 无法判定 / 其它 harness 错误
"""
from __future__ import annotations

import argparse
import os
import sys


def _default_harness() -> str:
    env = os.environ.get("INDURTDB_DIAG_HARNESS")
    return env or "irt_diag_harness"


def _run_inspect(args):
    from .inspect import main_inspect
    return main_inspect(args)


def _run_smoke(args):
    from .smoke import main_smoke
    return main_smoke(args)


def _run_leak(args):
    from .leak import main_leak
    return main_leak(args)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="irt-diag", description="InduRTDB 运行时诊断工具")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_ins = sub.add_parser("inspect", help="巡检段健康状态")
    p_ins.add_argument("--id", required=True, help="实例 id")
    p_ins.set_defaults(func=_run_inspect)

    p_smoke = sub.add_parser("smoke", help="冒烟：起服务→写→读→订阅→停")
    p_smoke.add_argument("--harness", default=_default_harness())
    p_smoke.add_argument("--id", default="diag_smoke")
    p_smoke.add_argument("--points", type=int, default=64)
    p_smoke.add_argument("--subs", type=int, default=4)
    p_smoke.set_defaults(func=_run_smoke)

    p_leak = sub.add_parser("leak", help="fd 泄漏检测（反复启停）")
    p_leak.add_argument("--harness", default=_default_harness())
    p_leak.add_argument("--id", default="diag_leak")
    p_leak.add_argument("--cycles", type=int, default=20)
    p_leak.set_defaults(func=_run_leak)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
