/**
 * @file basic_example.c
 * @brief InduRTDB 基本使用示例 (C11)
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include <indurtdb/indurtdb.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("=== InduRTDB 基本使用示例 (C) ===\n");

    printf("1. 初始化数据库...\n");
    if (indurtdb_initialize("example_instance", 100, 10) != 0) {
        fprintf(stderr, "初始化失败: %s\n", indurtdb_get_last_error());
        return 1;
    }
    printf("   OK\n");

    printf("2. 写入数据...\n");
    indurtdb_write_double(1001, 23.5);
    indurtdb_write_bool(2001, true);
    indurtdb_write_int32(3001, -7);
    indurtdb_write_string(4001, "HVAC-01");
    printf("   OK, write_count=%llu\n",
           (unsigned long long)indurtdb_get_write_count());

    printf("3. 读取数据...\n");
    indurtdb_point_t p;
    if (indurtdb_read_point(1001, &p) == 0) {
        printf("   温度: %.2f, quality=%d\n", p.value.d, (int)p.quality);
    }

    printf("4. peek (seqlock 保护, 线程本地缓冲)...\n");
    const indurtdb_point_t* pk = indurtdb_peek(1001);
    if (pk) printf("   温度 (peek): %.2f\n", pk->value.d);

    printf("5. 关闭...\n");
    indurtdb_shutdown();

    printf("=== 示例运行完成! ===\n");
    return 0;
}
