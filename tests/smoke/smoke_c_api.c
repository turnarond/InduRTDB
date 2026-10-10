/**
 * @file smoke_c_api.c
 * @brief InduRTDB C ABI 冒烟测试
 * @version 1.0.0
 * @date 2026-09-17
 * @copyright MIT License
 *
 * 覆盖 C ABI 全部公开函数：initialize / shutdown / write_* / read_* /
 *             read_point / validate_id / mark_timeout / get_write_count /
 *             get_timeout_count / get_subscriber_count / get_last_error
 *
 * 不依赖 GTest，独立可执行文件，返回 0 表示全部通过，非 0 表示失败。
 */

#include <indurtdb/api/c/indurtdb_c.h>

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static int g_failures = 0;

static void check(bool condition, const char* msg) {
    if (condition) {
        printf("  [PASS] %s\n", msg);
    } else {
        printf("  [FAIL] %s\n", msg);
        ++g_failures;
    }
}

int main(void) {
    printf("=== InduRTDB C ABI 冒烟测试 ===\n\n");

    /* ---------- 阶段 1: 初始化 ---------- */
    printf("[1] 初始化数据库\n");
    int rc = indurtdb_initialize("smoke_c_api", 4096, 16);
    check(rc == 0, "indurtdb_initialize() 返回 0 (成功)");
    if (rc != 0) {
        printf("\n[FATAL] 初始化失败: %s\n", indurtdb_get_last_error());
        return 1;
    }

    /* ---------- 阶段 2: 写入全部数据类型 ---------- */
    printf("\n[2] 写入数据\n");
    check(indurtdb_write_double(1001, 23.5) == 0,       "write_double(1001, 23.5)");
    check(indurtdb_write_int32(1002, 42) == 0,          "write_int32(1002, 42)");
    check(indurtdb_write_bool(1003, true) == 0,         "write_bool(1003, true)");
    check(indurtdb_write_string(1004, "HVAC-01") == 0,  "write_string(1004, \"HVAC-01\")");

    /* ---------- 阶段 3: 按类型读取 ---------- */
    printf("\n[3] 按类型读取\n");
    {
        double v = 0.0;
        check(indurtdb_read_double(1001, &v) == 0, "read_double(1001) 成功");
        check(v == 23.5, "read_double(1001) 值 == 23.5");
    }
    {
        int32_t v = 0;
        check(indurtdb_read_int32(1002, &v) == 0, "read_int32(1002) 成功");
        check(v == 42, "read_int32(1002) 值 == 42");
    }
    {
        bool v = false;
        check(indurtdb_read_bool(1003, &v) == 0, "read_bool(1003) 成功");
        check(v == true, "read_bool(1003) 值 == true");
    }
    {
        char buf[64] = {0};
        check(indurtdb_read_string(1004, buf, sizeof(buf)) == 0,
              "read_string(1004) 成功");
        check(strcmp(buf, "HVAC-01") == 0, "read_string(1004) 值 == \"HVAC-01\"");
    }

    /* ---------- 阶段 4: 读取完整点位 ---------- */
    printf("\n[4] 读取完整点位 (read_point)\n");
    {
        indurtdb_point_t p;
        memset(&p, 0, sizeof(p));
        check(indurtdb_read_point(1001, &p) == 0, "read_point(1001) 成功");
        check(p.value.d == 23.5, "read_point(1001).value.d == 23.5");
        check(p.timestamp_ns > 0, "read_point(1001).timestamp_ns > 0");
        /* type: DOUBLE == 2 */
        check(p.type == 2, "read_point(1001).type == DOUBLE(2)");
        /* quality: GOOD == 0 */
        check(p.quality == 0, "read_point(1001).quality == GOOD(0)");
    }

    /* ---------- 阶段 5: ID 校验 ---------- */
    printf("\n[5] ID 合法性校验\n");
    check(indurtdb_validate_id(1001) == 0, "validate_id(1001) 合法");
    check(indurtdb_validate_id(999999) != 0, "validate_id(超大值) 非法");

    /* ---------- 阶段 6: 统计与错误 ---------- */
    printf("\n[6] 统计信息与错误处理\n");
    uint64_t count_before = indurtdb_get_write_count();
    check(count_before >= 4, "get_write_count() >= 4 (已写入 4 个点位)");

    /* 读取未写入点位应失败并设置错误 */
    double dummy;
    check(indurtdb_read_double(8888, &dummy) != 0,
          "read_double(未写入 ID) 返回非 0");
    const char* err = indurtdb_get_last_error();
    check(err != NULL && strlen(err) > 0, "get_last_error() 返回非空错误信息");

    /* 再次写入增加计数 */
    indurtdb_write_double(2001, 1.0);
    check(indurtdb_get_write_count() == count_before + 1,
          "write 后 write_count +1");

    /* ---------- 阶段 7: 超时标记与订阅统计 ---------- */
    printf("\n[7] 超时标记与订阅统计\n");
    check(indurtdb_get_timeout_count() == 0, "get_timeout_count() 初始为 0");
    check(indurtdb_mark_timeout(1001) == 0, "mark_timeout(1001) 成功");
    check(indurtdb_get_timeout_count() == 1, "get_timeout_count() 递增为 1");
    check(indurtdb_mark_timeout(999999) != 0, "mark_timeout(越界 ID) 返回非 0");
    {
        indurtdb_point_t p;
        memset(&p, 0, sizeof(p));
        check(indurtdb_read_point(1001, &p) == 0, "mark_timeout 后点位仍可读");
        /* quality: TIMEOUT == 2 */
        check(p.quality == 2, "read_point(1001).quality == TIMEOUT(2)");
        check(p.value.d == 23.5, "mark_timeout 不破坏已采集的值");
    }
    check(indurtdb_get_subscriber_count() == 0,
          "get_subscriber_count() 为 0 (无订阅)");

    /* ---------- 阶段 8: 关闭 ---------- */
    printf("\n[8] 关闭数据库\n");
    indurtdb_shutdown();
    printf("  [INFO] indurtdb_shutdown() 已调用\n");

    /* ---------- 结果汇总 ---------- */
    printf("\n========================================\n");
    if (g_failures == 0) {
        printf("C ABI 冒烟测试结果: 全部通过 (PASS)\n");
        printf("========================================\n");
        return 0;
    } else {
        printf("C ABI 冒烟测试结果: %d 项失败 (FAIL)\n", g_failures);
        printf("========================================\n");
        return 1;
    }
}
