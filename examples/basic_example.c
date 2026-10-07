/**
 * @file basic_example.c
 * @brief InduRTDB 基本使用示例 (C11)
 * @version 3.7.0
 * @date 2026-07-15
 * @copyright MIT License
 *
 * v3.7 (主题B B5)：迁移到 **v2 句柄 API** (`indurtdb_h_*`)。
 * v1 全局 `indurtdb_*` 函数自 v3.7 起标注弃用（仍可用，v4.0 移除）。
 */

#include <indurtdb/indurtdb.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    printf("=== InduRTDB 基本使用示例 (C, API v2 句柄) ===\n");

    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 100, .max_subscribers = 10 };

    printf("1. 打开实例...\n");
    if (indurtdb_h_open(&db, "example_instance", &cfg) != 0) {
        fprintf(stderr, "打开失败: %s\n", indurtdb_get_last_error());
        return 1;
    }
    printf("   OK\n");

    printf("2. 写入数据...\n");
    indurtdb_h_write_double(db, 1001, 23.5);
    indurtdb_h_write_bool(db, 2001, true);
    indurtdb_h_write_int32(db, 3001, -7);
    indurtdb_h_write_string(db, 4001, "HVAC-01");
    printf("   OK, write_count=%llu\n",
           (unsigned long long)indurtdb_h_get_write_count(db));

    printf("3. 读取数据...\n");
    indurtdb_point_t p;
    if (indurtdb_h_read_point(db, 1001, &p) == 0) {
        printf("   温度: %.2f, quality=%d\n", p.value.d, (int)p.quality);
    }

    printf("4. peek (seqlock 保护, 线程本地缓冲)...\n");
    const indurtdb_point_t* pk = indurtdb_h_peek(db, 1001);
    if (pk) printf("   温度 (peek): %.2f\n", pk->value.d);

    printf("5. 关闭...\n");
    indurtdb_h_close(db);

    printf("=== 示例运行完成! ===\n");
    return 0;
}
