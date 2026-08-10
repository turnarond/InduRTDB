# InduRTDB — Industrial Real-Time Database

**纯 C11 实现** 的工业实时数据库,面向边缘控制的超低延迟、多进程共享内存数据中枢。

## 定位与边界

**InduRTDB 是工业级"共享内存实时数据层"** —— 一个纯 C、跨进程、确定性的内存点位存储库。它是数据链路**中间的实时热数据层**，不是整条链路。

| 做 | 不做 |
|---|---|
| 跨进程共享内存实时点位存储 (低延迟、确定性) | 南向设备接入 (Modbus/OPC UA/S7 驱动) |
| 工业语义 (quality / timestamp / unit / access) | 北向 SCADA 对接 (OPC UA Server / MQTT 上送) |
| 多进程读写、订阅、崩溃自愈 | 持久化 / 历史库 |
| 作为库被集成 (find_package / pkg-config / FetchContent) | 集群 / 跨节点同步 |

**与 node-server 的关系:** InduRTDB 作为 [node-server](https://github.com/acoinfo/edge-framework)（BAS Edge Data Hub）的**底层数据层**,提供跨进程共享内存实时能力;北向 OPC UA、持久化等能力复用 node-server 既有实现,不重复造轮子。

### x86 实测性能 (v3.1.0, Release -O2)

| 操作 | P50 | P99 | 吞吐 |
|------|-----|-----|------|
| write_int32 | 0.156 μs | 0.386 μs | 5.2M op/s |
| read_int32 | 0.062 μs | 0.068 μs | 16.2M op/s |
| peek (零拷贝) | 0.057 μs | 0.062 μs | 17.7M op/s |
| read_range (100点) | 2.893 μs | 2.992 μs | 35.5M pt/s |
| write_range (100点) | 12.459 μs | 17.911 μs | 8.1M pt/s |
| mixed_rw | -- | -- | 16.3M op/s |

> 设计目标 P99 ≤ 10 μs, x86 实测全部通过。ARM 平台实测待硬件到位后补充。
> 详见 `tests/bench/bench.c` + `scripts/run_bench.sh`。

## 快速开始 (C)

```c
#include <indurtdb/indurtdb.h>

int main(void) {
    // 初始化 (单例)
    indurtdb_initialize("my_instance", 50000, 32);

    // 写入
    indurtdb_write_double(1001, 23.5);       // 温度
    indurtdb_write_bool(2001, true);         // 水泵

    // 读取
    indurtdb_point_t p;
    indurtdb_read_point(1001, &p);
    printf("温度: %.2f\n", p.value.d);

    // peek (零拷贝)
    const indurtdb_point_t* pk = indurtdb_peek(1001);

    // 批量
    indurtdb_point_t buf[10];
    int n = indurtdb_read_range(0, 10, buf, 10);

    // 清理
    indurtdb_shutdown();
}
```

## 构建

```bash
mkdir build && cd build
cmake .. && make -j$(nproc)
ctest --output-on-failure
```

## 版本

v3.1.0 — 纯 C11 重写,C++ API 已移除。共享内存布局与 v2.x 逐字节兼容。

## 文档

- [集成指南](docs/05-SDK手册/04-集成指南.md) — find_package / pkg-config / FetchContent 三种集成方式
- [完整文档目录](docs/README.md) — 白皮书、设计、API 参考等

## 许可证

MIT License
