/**
 * @file stats.h
 * @brief rtdbd 运行统计与健康自检（B1，v3.7 主题B）
 *
 * 设计约束：
 * - 定长计数器单例，零堆分配（满足嵌入式「热路径零堆」不变式）
 * - 埋点只做 ++ 计数，绝不写日志/不阻塞（可在 poll 循环内直接调用）
 * - 信号处理器只置 volatile sig_atomic_t 标志，绝不直接 dump（见 rtdbd.c on_signal）
 *
 * 注：本文件位于 rtdbd/src/ 而非计划书所写的 rtdbd/include/rtdbd/，
 *     以与同批次的 rbe.h 保持一致（rtdbd 内部私有头统一放 src/）。
 */
#ifndef RTDBD_STATS_H_
#define RTDBD_STATS_H_

#include <stdint.h>
#include <stdio.h>

#include <rtdbd/protocol.h>   /* RTDBD_HEALTH_*：单一事实来源，杜绝数值漂移 */

#ifdef __cplusplus
extern "C" {
#endif

/* 错误计数分类：rtdbd 内部各类可观测故障的累加点 */
typedef struct {
    uint64_t n_writes;          /* 成功写次数（do_write 返回 0） */
    uint64_t n_notifies;        /* 入队通知数（=广播次数；含触发丢弃的入队，不扣 notify_drop） */
    uint64_t decode_fail;       /* 请求解析失败：magic/version 不符、payload 长度不符 */
    uint64_t write_rejected;    /* 鉴权拒绝（policy deny） */
    uint64_t write_error;       /* do_write 返回非 0 且非 -99（类型不支持） */
    uint64_t notify_drop;       /* 通知丢弃数（队列满丢最旧 + 连接关闭丢弃待发队列） */
    uint64_t notify_send_fail;  /* 出站发送失败（对端已关闭等） */
    uint64_t uptime_ns;         /* 最近一次快照时的进程启动至今纳秒 */
    int      health;            /* 最近一次自检结果：RTDBD_HEALTH_* */
} rtdbd_stats_t;

/* 计数器单例（定长，进程内唯一） */
extern rtdbd_stats_t g_stats;

/* 记录进程启动时刻（main 早期调用一次），供 uptime 计算 */
void rtdbd_stats_mark_start(void);

/* 错误类事件统一入口：除累加对应计数外，刷新「最近错误时刻」，
 * 供健康判定的 60s 滑动窗口使用（健康状态因此可自愈，不会永久锁存 DEGRADED）。 */
void rtdbd_stats_note_error(void);

/* 自检（核心段 + 计数分类，由 stats.c 实现，不依赖连接表）。
 * 返回 RTDBD_HEALTH_*，并写入 g_stats.health。
 * 规则：核心段损坏 → UNHEALTHY；仅错误计数非零（有被拒写/丢通知等）→ DEGRADED；
 *       一切正常 → OK。 */
int rtdbd_self_check_core(void);

/* 文本化导出（SIGUSR1 dump 走此路径，在主循环内调用，非信号上下文） */
void rtdbd_stats_dump(FILE* fp);

#ifdef __cplusplus
}
#endif

#endif /* RTDBD_STATS_H_ */
