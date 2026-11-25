/**
 * @file rtdbd_notify_latency.c
 * @brief S4 VSOA NOTIFY 延迟旁路环实现（见 rtdbd_notify_latency.h）。
 */
#include "rtdbd_notify_latency.h"

#include <string.h>
#include <time.h>

typedef struct {
    uint32_t point_id;
    uint64_t t0_ns;
    uint64_t write_seq;
    uint32_t sample_count;
} latency_entry_t;

static latency_entry_t g_entries[RTDBD_NOTIFY_LATENCY_CAP];
static uint32_t g_head = 0u;       /* 下一个写入位置（环形） */
static uint32_t g_used = 0u;
static uint64_t g_next_seq = 0u;
static rtdbd_notify_latency_stats_t g_stats = {0u, 0u, 0u, 0u, 0u, "vsoa"};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void stats_init(void)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memcpy(g_stats.transport, "vsoa", 5); /* 含 '\0' */
}

uint64_t rtdbd_notify_latency_t0(uint32_t point_id)
{
    latency_entry_t *e = &g_entries[g_head];
    if (g_used == RTDBD_NOTIFY_LATENCY_CAP && e->sample_count == 0u) {
        g_stats.dropped_unpaired++; /* 覆盖未配对条目：记丢弃（防静默丢样本） */
    }
    g_next_seq++;
    if (g_next_seq == 0u) g_next_seq = 1u; /* 跳过回绕 0 */
    e->point_id = point_id;
    e->t0_ns = now_ns();
    e->write_seq = g_next_seq;
    e->sample_count = 0u;
    g_head = (g_head + 1u) % RTDBD_NOTIFY_LATENCY_CAP;
    if (g_used < RTDBD_NOTIFY_LATENCY_CAP) g_used++;
    return g_next_seq;
}

uint64_t rtdbd_notify_latency_t1(uint32_t point_id)
{
    if (g_used == 0u) return 0u;
    /* 从最新向旧查同 point 的最新条目（不判断 sample_count——一 t0 多 t1 多样本）。 */
    for (uint32_t k = 0; k < g_used; ++k) {
        uint32_t idx = (g_head + RTDBD_NOTIFY_LATENCY_CAP - 1u - k) % RTDBD_NOTIFY_LATENCY_CAP;
        latency_entry_t *e = &g_entries[idx];
        if (e->point_id != point_id) continue;
        uint64_t t1 = now_ns();
        uint64_t delta = t1 > e->t0_ns ? t1 - e->t0_ns : 0u;
        e->sample_count++;
        g_stats.count++;
        if (delta > g_stats.max_ns) g_stats.max_ns = delta;
        g_stats.last_ns = delta;
        g_stats.last_write_seq = e->write_seq;
        return delta;
    }
    return 0u; /* 无同点 t0：不计样本 */
}

rtdbd_notify_latency_stats_t rtdbd_notify_latency_stats(void)
{
    return g_stats;
}

void rtdbd_notify_latency_reset_for_test(void)
{
    memset(g_entries, 0, sizeof(g_entries));
    g_head = 0u;
    g_used = 0u;
    g_next_seq = 0u;
    stats_init();
}
