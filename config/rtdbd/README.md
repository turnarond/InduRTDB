# 配置样例

rtdbd 的配置样例，可直接复制为部署起点。

| 文件 | 对应参数 | 格式 |
|---|---|---|
| `policy.conf` | `--policy` | 自定义 `uid:min_id:max_id` 逐行格式（**非 YAML**） |
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

⚠️ `policy.conf` 中格式错误的行被**静默跳过**，启动不报错。自查：

```bash
grep -vE '^\s*(#|$)' /etc/rtdbd/policy.conf | wc -l   # 必须等于预期规则数
```

## 本样例的验证状态

`points.yaml` 的 9 个点位已实测通过「rtdbd 启动 → 写段 → 逐点回读」验证，
类型 / unit / access 三字段逐项核对正确。

| id | name | type | unit | access |
|---|---|---|---|---|
| 1001 | AHU_01.Supply_Temp | double(2) | 1 °C | 3 RW |
| 1002 | AHU_01.Supply_Temp_Humidity | double(2) | 3 % | 3 RW |
| 1003 | AHU_01.Supply_Pressure | double(2) | 2 Pa | 3 RW |
| 1004 | AHU_01.Energy_Total | int32(1) | 0 | 3 RW |
| 1005 | AHU_01.Coil_Valve_Position | double(2) | 3 % | 3 RW |
| 2001 | Pump_01.Start_CMD | bool(0) | 0 | 3 RW |
| 2002 | Fan_01.Speed_Setpoint | int32(1) | 0 | 3 RW |
| 3001 | Pump_01.Running_Status | bool(0) | 0 | **1 RO** |
| 3002 | AHU_01.Mode_Text | string(3) | 0 | **1 RO** |

## 已知限制：配置解析器不支持 int64 / uint32

`irt_config_parse_type()`（`src/core/irt_config.c:107`）只识别 **4 种**类型名：

```
bool
int32   （别名 int）
double  （别名 float —— 会被当成 double，不保留单精度）
string  （别名 str）
```

尽管 API 层定义了 `INDURTDB_TYPE_INT64(4)` 与 `INDURTDB_TYPE_UINT32(5)`，
配置里写 `type: int64` / `type: uint32` 会被解析为 `0xff`，
随后在 `src/api/indurtdb.c:367` 的类型过滤处被**静默丢弃**：

```c
if (pm->type > INDURTDB_TYPE_STRING) continue;
```

**rtdbd 仍以退出码 0 正常启动，该点位不会出现，`find_by_name` 报 NOT_FOUND。**
需要这两类点位只能在初始化后经 `SET_META` / 运行时 API 设定。

此为已知缺口，已登记在
[`06-开发规划/13-Linux产品化完善计划.md`](../../docs/06-开发规划/13-Linux产品化完善计划.md)。

## 验证命令

```bash
# 段巡检（version / 点数 / CRC / owner pid / index_count）
cd tools/irt_diag && python3 -m irt_diag.cli inspect --id <instance_id>

# index_count 应等于配置中实际生效的点位数
```

若 `index_count` 小于配置文件的条目数，说明有条目被静默丢弃——
最常见原因就是上面的 `type: int64` / `type: uint32`。
