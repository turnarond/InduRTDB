# InduRTDB Starter

引用 InduRTDB 的最小可运行工程，双模式。

## 方式一：find_package（需先安装 InduRTDB）

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=<InduRTDB 安装前缀>
cmake --build build
./build/starter
```

## 方式二：FetchContent（源码内嵌，无需安装）

```bash
# 离线(本地源码)
cmake -S . -B build -DSTARTER_USE_FETCHCONTENT=ON \
      -DINDURTDB_SOURCE_DIR=/path/to/InduRTDB
# 在线(远端仓库): 省略 INDURTDB_SOURCE_DIR
cmake --build build
./build/starter
```

预期输出：`starter ok: temp=23.5, on=1`
