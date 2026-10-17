/**
 * @file peek_lifetime_example.c
 * @brief indurtdb_peek() 生命周期示例 —— 演示「单拷贝」陷阱及其正确用法
 * @version 3.8.0
 * @copyright MIT License
 *
 * 为什么需要这个示例：
 *
 * indurtdb_peek() 常被误解为「零拷贝，返回共享内存指针」。**实际不是。**
 * 它把点位数据经 Seqlock 拷贝到**线程本地缓冲**，返回该缓冲的指针 ——
 * 因此**同线程的下一次 peek() 会覆盖上一次的结果**。
 *
 * 本示例实际跑一遍这个陷阱，让你看到覆盖前后的真实数值，而不是只听文档说。
 *
 * 结论：
 *   - 单次 peek 后立即消费            → 安全
 *   - 持有多个 peek 指针 / 跨调用使用 → 数据已被覆盖（错误）
 *   - 需要长期持有                   → 用 indurtdb_h_read_point()
 */

#include <indurtdb/indurtdb.h>
#include <stdio.h>

#define ID_TEMP 1
#define ID_HUMI 2

int main(void) {
    printf("=== peek 生命周期示例 (API v2 句柄) ===\n\n");

    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 100, .max_subscribers = 10 };
    if (indurtdb_h_open(&db, "peek_example", &cfg) != 0) {
        fprintf(stderr, "打开失败: %s\n", indurtdb_get_last_error());
        return 1;
    }

    /* 两个值差异明显的点位，便于肉眼分辨覆盖是否发生 */
    indurtdb_h_write_double(db, ID_TEMP, 23.5);
    indurtdb_h_write_double(db, ID_HUMI, 88.8);

    /* ---- 1. 正确：单次 peek 后立即消费 ---- */
    printf("1. 单次 peek 后立即消费（正确）\n");
    const indurtdb_point_t* pk = indurtdb_h_peek(db, ID_TEMP);
    if (pk) {
        printf("   peek(%d) -> %.2f   ← 立即使用，拿到的是点位 %d 的值\n\n",
               ID_TEMP, pk->value.d, ID_TEMP);
    }

    /* ---- 2. 陷阱：持有两个 peek 指针 ---- */
    printf("2. 陷阱：连续 peek 两个点位，却用第一个的指针取值\n");
    printf("   实际数据：点位%d=23.5，点位%d=88.8\n\n", ID_TEMP, ID_HUMI);

    const indurtdb_point_t* a = indurtdb_h_peek(db, ID_TEMP);  /* 想留温度 */
    const indurtdb_point_t* b = indurtdb_h_peek(db, ID_HUMI);  /* 又读了湿度 */

    printf("   a = peek(%d), b = peek(%d)\n", ID_TEMP, ID_HUMI);
    printf("   a 指针 = %p, b 指针 = %p  %s\n\n",
           (const void*)a, (const void*)b,
           (a == b) ? "<-- 同一个缓冲！peek 是单拷贝，不是零拷贝" : "");

    printf("   期望 a->value.d == %.2f（点位%d 的温度）\n", 23.5, ID_TEMP);
    printf("   实测 a->value.d == %.2f  %s\n\n",
           a ? a->value.d : 0.0,
           (a && a->value.d != 23.5)
               ? "<-- 已被 b 的 peek 覆盖！拿到的是点位 2 的值"
               : "");

    /* ---- 3. 正确做法 A：需要长期持有 → read_point ---- */
    printf("3. 正确做法 A：需要长期持有时用 read_point（拷贝到自己的缓冲）\n");
    indurtdb_point_t keep;
    if (indurtdb_h_read_point(db, ID_TEMP, &keep) == 0) {
        /* 中间做任意多次 peek / read 都不影响 keep */
        indurtdb_h_peek(db, ID_HUMI);
        indurtdb_h_peek(db, ID_TEMP);
        printf("   中途做了 2 次 peek 后，keep.value.d 仍是 %.2f  ← 稳定\n\n",
               keep.value.d);
    }

    /* ---- 4. 正确做法 B：批量遍历时逐个 peek + 立即消费 ---- */
    printf("4. 正确做法 B：批量遍历时逐个 peek 并立即消费\n");
    double total = 0.0;
    for (uint32_t id = ID_TEMP; id <= ID_HUMI; ++id) {
        const indurtdb_point_t* cur = indurtdb_h_peek(db, id);
        if (!cur) continue;
        total += cur->value.d;          /* 关键：取到值马上累加，不存指针 */
        printf("   peek(%u) -> %.2f\n", id, cur->value.d);
    }
    printf("   合计 %.2f（= 23.5 + 88.8，说明两次都取对了）\n\n", total);

    indurtdb_h_close(db);
    printf("=== 完成 ===\n");
    return 0;
}