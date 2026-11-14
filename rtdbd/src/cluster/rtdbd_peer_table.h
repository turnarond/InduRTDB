/**
 * @file rtdbd_peer_table.h
 * @brief v3.12 IR4：集群成员运行时表（doc 13 §8.3-6）。
 *
 * 设计约束：
 * - `rtdbd_peer_t { addr[64], node_id[32], state, boot_id, applied_offset, lag }`
 *   **定长 `peers[32]`、零动态内存**，只留 rtdbd 进程内（§8.3-6）。
 * - 以 `SET_PEERS` **整组替换**为主，另给 `ADD` / `DEL` / `GET`。
 * - 重复 IP 去重、拒绝自地址 / 非法地址 / 超上限 → `BAD_REQUEST` 且**旧表不变**
 *   （原子替换：先在临时表构建完整集合，全部通过后再交换）。
 * - ADD / DEL 由上层走**延迟动作队列**（§8.3-3）；新 peer 触发 `on_join` 全量，
 *   本模块只负责「是否新增」的返回，不直接执行动作。
 *
 * 纯逻辑模块：不依赖 socket / fd / VSOA / rtdbd 全局状态，故默认构建即可单测。
 */
#ifndef RTDBD_PEER_TABLE_H_
#define RTDBD_PEER_TABLE_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* rtdbd_peer_t / RTDBD_PEER_* / 状态宏已提升为协议单一真源（方案评审结论）。 */
#include <rtdbd/protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    rtdbd_peer_t peers[RTDBD_PEER_MAX];
    uint32_t     count;
} rtdbd_peer_table_t;

void rtdbd_peer_table_init(rtdbd_peer_table_t *t);

/* 整组替换（原子）。返回 0 成功；-1 表示参数非法（自地址/非法地址/超限/重复冲突）。
 * ⚠ 失败时 **旧表保持不变**（§8.3-6 硬要求）。
 * `self_addr` 为本节点地址（判「拒绝自地址」用）；可传 NULL 表示不校验自地址。 */
int rtdbd_peer_table_set(rtdbd_peer_table_t *t, const rtdbd_peer_t *peers, uint32_t n,
                          const char *self_addr);

/* 增 / 删 / 查。add 返回 0=新增成功、1=已存在（幂等）、-1=非法或表满。
 * 新增成功即「新 peer 加入」，由上层入延迟动作队列触发 on_join 全量。 */
int rtdbd_peer_table_add(rtdbd_peer_table_t *t, const rtdbd_peer_t *p,
                          const char *self_addr);
int rtdbd_peer_table_del(rtdbd_peer_table_t *t, const char *addr);
const rtdbd_peer_t *rtdbd_peer_table_get(const rtdbd_peer_table_t *t, const char *addr);

/* 地址合法性（非空、长度合法、IPv4 点分十进制）。纯函数，供上层与单测复用。 */
bool rtdbd_peer_addr_valid(const char *addr);

/* 最小 lag 计算（§8.3-6 lag 精确语义未裁决）：lag = (local > applied) ? (local - applied) : 0。
 * 抽为纯函数，供 rtdbd_peers_apply.c 与单测共用，避免依赖全局状态。 */
static inline uint32_t rtdbd_peer_lag(uint64_t local_offset, uint64_t applied_offset)
{
    return (local_offset > applied_offset) ? (uint32_t)(local_offset - applied_offset) : 0u;
}

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_PEER_TABLE_H_ */
