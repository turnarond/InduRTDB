/**
 * @file rtdbd_notify_latency.h
 * @brief S4：VSOA NOTIFY 延迟旁路环（写生效→NOTIFY 发出，外部打点不改 POD）。
 *
 * 纯内存、单线程无锁；条目 {point_id,t0_ns,write_seq,sample_count} 纯追加覆盖最旧。
 * 一写多 NOTIFY：同一 write_seq 被 N 个客户端各消费一次 → N 样本（用户裁决）。
 * 不碰 rtdbd_notify_t 32B / notify_counters 32B / health 72B 冻结 POD。
 */
#ifndef RTDBD_NOTIFY_LATENCY_H_
#define RTDBD_NOTIFY_LATENCY_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_NOTIFY_LATENCY_CAP 256u

typedef struct {
    uint64_t count;            /* 实际 t0+t1 配对样本数（= NOTIFY 发送次数） */
    uint64_t max_ns;
    uint64_t last_ns;
    uint64_t last_write_seq;   /* 可校验配对证据 */
    uint64_t dropped_unpaired; /* 覆盖时 sample_count==0 的条目数 */
    char     transport[8];     /* "vsoa"：避免 S5 误认两前端统一口径 */
} rtdbd_notify_latency_stats_t;

/* S4：现场取证 POD 尺寸冻结（48B：5×u64 + 8B transport）。 */
#ifdef __cplusplus
static_assert(sizeof(rtdbd_notify_latency_stats_t) == 48u,
              "S4 stats POD must stay 48B (5*8 + 8)");
#else
_Static_assert(sizeof(rtdbd_notify_latency_stats_t) == 48u,
               "S4 stats POD must stay 48B (5*8 + 8)");
#endif

/* t0：VSOA 写成功后追加，返回本次 write_seq（全局单调、非零）。 */
uint64_t rtdbd_notify_latency_t0(uint32_t point_id);
/* t1：按最新同 point 条目配对，返回 t1-t0（ns）；无条目返回 0（不计样本）。 */
uint64_t rtdbd_notify_latency_t1(uint32_t point_id);
rtdbd_notify_latency_stats_t rtdbd_notify_latency_stats(void);

/* 仅供单测：重置全局环与聚合（非生产 API）。 */
void rtdbd_notify_latency_reset_for_test(void);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_NOTIFY_LATENCY_H_ */
