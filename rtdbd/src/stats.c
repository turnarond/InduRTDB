/**
 * @file stats.c
 * @brief rtdbd 运行统计与健康自检实现（B1，v3.7 主题B）
 *
 * 计数器单例定长无堆；埋点只做 ++。本文件不持有连接表（连接表是 rtdbd.c 的
 * 全局 g_conns），以避免 stats 反向依赖 rtdbd 的内部连接结构。
 */
#include "stats.h"

#include <indurtdb/indurtdb.h>
#include <time.h>

/* 计数器单例：文件域静态，零堆分配 */
rtdbd_stats_t g_stats;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 进程启动时刻（CLOCK_MONOTONIC），mark_start 记录一次 */
static uint64_t g_start_ns;

/* 最近一次「错误类事件」的时刻（CLOCK_MONOTONIC）。
 * 健康判定用滑动窗口而非累计值：单次瞬时误请求不应让进程余生都是
 * DEGRADED（信号一旦不具区分度就等于没有信号，必然演变成告警疲劳）。 */
static uint64_t g_last_err_ns;

/* DEGRADED 判定窗口：最近该时间窗内出现过错误类事件即降级。
 * 取 60s：足以覆盖一次瞬时误操作，又能让稳态无错的进程自动回落到 OK。 */
#define RTDBD_DEGRADED_WINDOW_NS (60ull * 1000000000ull)

/* 错误类事件统一入口：刷新最近错误时刻（供健康窗口判定）。 */
void rtdbd_stats_note_error(void)
{
    g_last_err_ns = now_ns();
}

void rtdbd_stats_mark_start(void)
{
    g_start_ns = now_ns();
}

/* 依据「最近窗口内是否有错误」分类健康，可自愈：
 *   - 最近 DEGRADED_WINDOW_NS 内出现过错误类事件 → DEGRADED；
 *   - 窗口内无新错误 → 自动回落到 OK。
 * 累计计数仍完整保留在 g_stats 中（供差分与告警判据），但不用于锁存状态。 */
static int classify(void)
{
    if (g_last_err_ns == 0) return RTDBD_HEALTH_OK;
    uint64_t now = now_ns();
    if (now < g_last_err_ns) return RTDBD_HEALTH_OK;          /* 时钟异常，保守判 OK */
    if (now - g_last_err_ns > RTDBD_DEGRADED_WINDOW_NS) return RTDBD_HEALTH_OK;
    return RTDBD_HEALTH_DEGRADED;
}

int rtdbd_self_check_core(void)
{
    int core = indurtdb_self_check();   /* 段头 magic/version，纯读 */
    if (core == INDURTDB_HEALTH_UNHEALTHY) {
        g_stats.health = RTDBD_HEALTH_UNHEALTHY;
    } else {
        g_stats.health = classify();
    }
    g_stats.uptime_ns = g_start_ns ? (now_ns() - g_start_ns) : 0;
    return g_stats.health;
}

void rtdbd_stats_dump(FILE* fp)
{
    if (!fp) return;
    g_stats.uptime_ns = g_start_ns ? (now_ns() - g_start_ns) : 0;
    fprintf(fp, "# rtdbd stats\n");
    fprintf(fp, "health            = %d\n", g_stats.health);
    fprintf(fp, "uptime_ns         = %llu\n", (unsigned long long)g_stats.uptime_ns);
    fprintf(fp, "n_writes          = %llu\n", (unsigned long long)g_stats.n_writes);
    fprintf(fp, "n_notifies        = %llu\n", (unsigned long long)g_stats.n_notifies);
    fprintf(fp, "decode_fail       = %llu\n", (unsigned long long)g_stats.decode_fail);
    fprintf(fp, "write_rejected    = %llu\n", (unsigned long long)g_stats.write_rejected);
    fprintf(fp, "write_error       = %llu\n", (unsigned long long)g_stats.write_error);
    fprintf(fp, "notify_drop       = %llu\n", (unsigned long long)g_stats.notify_drop);
    fprintf(fp, "notify_send_fail  = %llu\n", (unsigned long long)g_stats.notify_send_fail);
}
