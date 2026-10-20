# 配置样例

rtdbd 的配置样例，可直接复制为部署起点。

| 文件 | 对应参数 | 格式 |
|---|---|---|
| `policy.conf` | `--policy` | 自定义 `uid:partition\|*:min_id:max_id` 逐行格式（**非 YAML**） |
| `points.yaml` | `--config` | YAML 子集，仅 `points:` 之后被解析 |

## 用法

```bash
sudo install -d /etc/rtdbd
sudo cp config/rtdbd/*.conf config/rtdbd/*.yaml /etc/rtdbd/

rtdbd --instance hvac_ahu01 \
      --socket /run/rtdbd/hvac_ahu01.sock \
      --policy /etc/rtdbd/policy.conf \
      --config /etc/rtdbd/points.yaml \
      --max-points 10000 \
      --max-subs 32
```

⚠️ **`--instance` / `--max-points` / `--max-subs` 只能走命令行。**
写在 `points.yaml` 里的同名键会被**静默忽略**（解析器只读 `points:` 之后的内容）。

⚠️ **`policy.conf` 不给等于关闭所有写。** 详见 [`02-启动与守护.md` §2](../../docs/07-部署运维/02-启动与守护.md)。

⚠️ `policy.conf` 格式错误即 **fail-fast**：任何一行不合规（段数不是 4、非十进制
数字、数值溢出 `uint32`、`min_id > max_id`、单行超长、规则数超 32），rtdbd 打印
`invalid policy <path>: line <n>: <原因>` 并以**退出码 2** 终止——**不再静默跳过**。
（v3.9 T3 之前的版本会把格式错误的行悄悄跳过，坏策略等同「拒绝所有写」却启动成功。）

## 本样例的验证状态

`points.yaml` 的 11 个点位已实测通过「rtdbd 启动 → 写段 → 逐点回读」验证，
类型 / unit / access 三字段逐项核对正确。

| id | name | type | unit | access |
|---|---|---|---|---|
| 1001 | AHU_01.Supply_Temp | double(2) | 1 °C | 3 RW |
| 1002 | AHU_01.Supply_Temp_Humidity | double(2) | 3 % | 3 RW |
| 1003 | AHU_01.Supply_Pressure | double(2) | 2 Pa | 3 RW |
| 1004 | AHU_01.Energy_Total | int32(1) | 0 | 3 RW |
| 1005 | AHU_01.Coil_Valve_Position | double(2) | 3 % | 3 RW |
| 1006 | AHU_01.Energy_Total_Wh | **int64(4)** | 0 | 3 RW |
| 1007 | AHU_01.Run_Count | **uint32(5)** | 0 | 3 RW |
| 2001 | Pump_01.Start_CMD | bool(0) | 0 | 3 RW |
| 2002 | Fan_01.Speed_Setpoint | int32(1) | 0 | 3 RW |
| 3001 | Pump_01.Running_Status | bool(0) | 0 | **1 RO** |
| 3002 | AHU_01.Mode_Text | string(3) | 0 | **1 RO** |

## 类型名解析

`irt_config_parse_type()`（`src/core/irt_config.c`）识别：

```
bool
int32   （别名 int）
int64
uint32
double  （别名 float —— 会被当成 double，不保留单精度）
string  （别名 str）
```

无法识别的名字返回 `0xff`，该点位会被**静默丢弃**。

`float` 映射到 `double` 是历史行为：`INDURTDB_TYPE_FLOAT(6)` 虽是类型集合的上限，
但解析器不产出该值——改动会让既有配置的点位静默降为单精度，故维持原状。

> **v3.8 修复**：此前解析器只认 4 种类型，`int64` / `uint32` 会被解析成 `0xff`，
> 再在 `src/api/indurtdb.c` 的 `if (pm->type > INDURTDB_TYPE_FLOAT) continue;` 处被丢弃，
> 而 **rtdbd 仍以退出码 0 正常启动**。已修复并加回归测试。
