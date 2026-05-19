/**
 * @file c_example.c
 * @brief InduRTDB C API 使用示例
 * @version 1.0.0
 * @date 2026-05-17
 * @copyright MIT License
 */

#include <indurtdb/api/c/indurtdb_c.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("=== InduRTDB C API 使用示例 ===\n\n");

    /* 1. 初始化 */
    printf("1. 初始化数据库...\n");
    int ret = indurtdb_initialize("c_example", 4096, 10);
    if (ret != 0) {
        fprintf(stderr, "初始化失败: %s\n", indurtdb_get_last_error());
        return 1;
    }
    printf("   ok 初始化成功\n\n");

    /* 2. 写入四种类型 */
    printf("2. 写入数据...\n");
    indurtdb_write_double(1001, 23.5);
    printf("   ok 写入 double: id=1001, value=23.5\n");

    indurtdb_write_int32(2001, 42);
    printf("   ok 写入 int32:  id=2001, value=42\n");

    indurtdb_write_bool(3001, 1);
    printf("   ok 写入 bool:   id=3001, value=true\n");

    indurtdb_write_string(4001, "Pump_01");
    printf("   ok 写入 string: id=4001, value=\"Pump_01\"\n\n");

    /* 3. 读取各类型 */
    printf("3. 读取数据...\n");

    double dval;
    if (indurtdb_read_double(1001, &dval) == 0) {
        printf("   温度: %.1f\n", dval);
    }

    int32_t ival;
    if (indurtdb_read_int32(2001, &ival) == 0) {
        printf("   计数器: %d\n", ival);
    }

    bool bval;
    if (indurtdb_read_bool(3001, &bval) == 0) {
        printf("   开关: %s\n", bval ? "ON" : "OFF");
    }

    char buf[32];
    if (indurtdb_read_string(4001, buf, sizeof(buf)) == 0) {
        printf("   设备名: %s\n", buf);
    }
    printf("\n");

    /* 4. 读取完整点位数据 */
    printf("4. 读取完整点位数据...\n");
    indurtdb_point_t point;
    if (indurtdb_read_point(1001, &point) == 0) {
        printf("   id=1001, value=%.1f, type=%d, quality=%d, timestamp=%lu\n",
               point.value.d, point.type, point.quality,
               (unsigned long)point.timestamp_ns);
    }
    printf("\n");

    /* 5. 统计信息 */
    printf("5. 统计信息...\n");
    printf("   总写入次数: %lu\n", (unsigned long)indurtdb_get_write_count());
    printf("\n");

    /* 6. 关闭 */
    printf("6. 关闭数据库...\n");
    indurtdb_shutdown();
    printf("   ok 关闭成功\n\n");

    printf("=== 示例运行完成 ===\n");
    return 0;
}
