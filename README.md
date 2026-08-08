# InduRTDB — Industrial Real-Time Database

**纯 C11 实现** 的工业实时数据库,面向边缘控制的超低延迟、多进程共享内存数据中枢。

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
