/**
 * @file rtdbd_sub_snapshot.h
 * @brief h-A 订阅真实重建：断链订阅点集快照（按 session_id 键，进程级单例）。
 *
 * 设计（变更卡 v3.12-hA订阅真实重建实现.md）：
 * - 断链时保存 session 的订阅点集（subs[]/nsubs），以服务端下发的稳定
 *   session_id 为键；重连 hello+rebind 时按 session_id 精确恢复。
 * - 只存点集 + 身份校验字段（uid/partition/service_id），**不存凭据**
 *   （s-A：uid/partition/role 每次 hello 重建；session_id 是重连标识非凭据）。
 * - 定长 32 条、每条按实际 n 受控分配（非 32×RTDBD_SUB_MAX 定长浪费），
 *   满则 LRU 淘汰最旧、计 evicted；分配失败计 dropped（不静默丢）。
 * - 单线程事件循环内使用（与 defer 队列同约束），非线程安全。
 */
#ifndef RTDBD_SUB_SNAPSHOT_H_
#define RTDBD_SUB_SNAPSHOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_SUB_SNAP_MAX 32u

/* 身份校验字段（纵深防御：rebind 时比对新 hello 重建的身份与快照记录一致）。 */
typedef struct {
    uint32_t uid;
    uint32_t partition;
    char     service_id[64];
} rtdbd_sub_snap_identity_t;

/* 保存点集快照。sid=服务端 session_id（非零）；ident=身份校验字段；
 * ids=点 id 数组（可 NULL 当 n=0）；n<=RTDBD_SUB_MAX。
 * 新 sid 若已存在则覆盖更新；表满淘汰最旧；分配失败返回 false 并计 dropped。 */
bool rtdbd_sub_snapshot_save(uint64_t sid, const rtdbd_sub_snap_identity_t *ident,
                             const uint32_t *ids, uint32_t n);

/* 加载快照：按 sid 命中且身份一致，拷贝点集到 out（容量 cap），返回 true；
 * out=NULL 或 cap<n 返回 false（不部分拷贝）。命中并用于恢复时刷新 LRU。 */
bool rtdbd_sub_snapshot_load(uint64_t sid, const rtdbd_sub_snap_identity_t *ident,
                             uint32_t *out, uint32_t cap, uint32_t *out_n);

/* 删除某 sid 快照（如 rebind 成功后消费一次，防重复恢复）。返回是否命中。 */
bool rtdbd_sub_snapshot_remove(uint64_t sid);

/* 进程级清空（测试/退出）。 */
void rtdbd_sub_snapshot_clear(void);

/* 诊断计数（可证伪：save 拒绝/淘汰/恢复命中/身份不符）。 */
typedef struct {
    uint64_t saved;       /* 累计成功保存 */
    uint64_t loaded;      /* 累计成功恢复 */
    uint64_t dropped;     /* 分配失败/参数非法被拒 */
    uint64_t evicted;     /* LRU 淘汰 */
    uint64_t identity_mismatch; /* sid 命中但身份不符（纵深防御拒绝） */
    uint64_t missed;      /* sid 未命中 */
} rtdbd_sub_snapshot_stats_t;

rtdbd_sub_snapshot_stats_t rtdbd_sub_snapshot_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_SUB_SNAPSHOT_H_ */
