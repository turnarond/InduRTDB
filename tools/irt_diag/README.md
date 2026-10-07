# irt-diag · InduRTDB 运行时诊断工具

纯 Python（`stdlib` only）的运行时诊断包，配合一个 C harness 完成段巡检、端到端冒烟与 fd 泄漏检测。可独立运行，亦可接入 CI。

## 目录结构

```
irt_diag/
├── irt_diag/            # Python 包
│   ├── _layout.py       # v2 Header 布局常量与解析（复刻 irt_types.h + CRC 区间）
│   ├── inspect.py       # 在线段巡检
│   ├── smoke.py         # 冒烟编排（驱动 harness）
│   ├── leak.py          # fd 泄漏检测（驱动 harness）
│   ├── cli.py           # 命令行入口
│   └── __main__.py
├── c_harness/           # C harness（由 CMake 构建为 irt_diag_harness）
│   └── smoke_leak.c     # 冒烟 + fd 泄漏检测
└── pyproject.toml
```

## 使用

```bash
# 巡检某实例段健康状态（版本/点数/CRC/owner pid）
python3 -m irt_diag.cli inspect --id <instance_id>

# 冒烟：起服务 → 写 → 读 → 订阅 → 停（需先构建 irt_diag_harness）
python3 -m irt_diag.cli smoke --harness <path>/irt_diag_harness --id <instance_id>

# fd 泄漏检测：反复启停 N 轮，比较 fd 计数
python3 -m irt_diag.cli leak --harness <path>/irt_diag_harness --id <instance_id> --cycles 20
```

退出码：`inspect` 0=健康 / 1=不健康 / 2=段不存在；`smoke` 透传 harness（0=各步骤成功）；`leak` 0=无泄漏 / 3=检测到 fd 增长 / 2=无法判定。

## 设计说明

- `_layout.py` 不依赖 C 库，离线/在线读取 `/dev/shm/indurtdb_<id>`，CRC 覆盖区间与
  `src/core/irt_shm.c:irt_header_crc32_of` 逐字节一致（[0:16] + [48:72]）。
- `scan_skipped` 位于保留区 [76:80]，**不**计入 CRC，故运行时自增不影响巡检 CRC。
- C harness 复用公共 API，零 C++ 依赖；泄漏检测通过 `/proc/self/fd` 目录计数实现。
