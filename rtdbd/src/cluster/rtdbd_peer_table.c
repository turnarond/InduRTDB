/**
 * @file rtdbd_peer_table.c
 * @brief v3.12 IR4：集群成员运行时表实现（doc 13 §8.3-6）。
 */
#include "rtdbd_peer_table.h"

#include <string.h>
#include <arpa/inet.h>

void rtdbd_peer_table_init(rtdbd_peer_table_t *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

bool rtdbd_peer_addr_valid(const char *addr)
{
    if (!addr || addr[0] == '\0' || strlen(addr) >= RTDBD_PEER_ADDR_MAX) {
        return false;
    }
    struct in_addr in4;
    /* 仅接受 IPv4 点分十进制（§8.3-6 成员表 addr[64] 的现场口径） */
    return inet_pton(AF_INET, addr, &in4) == 1;
}

/* 校验并规范化一条成员（返回 false 表示应整组拒绝）。 */
static bool peer_copy_checked(rtdbd_peer_t *dst, const rtdbd_peer_t *src,
                              const char *self_addr)
{
    if (!rtdbd_peer_addr_valid(src->addr)) {
        return false;
    }
    if (self_addr && strcmp(src->addr, self_addr) == 0) {
        return false; /* 拒绝自地址 */
    }
    if (strlen(src->node_id) >= RTDBD_PEER_NODE_ID_MAX) {
        return false;
    }
    memset(dst, 0, sizeof(*dst));
    /* 规范化 addr：inet_pton 接受前导零等非规范形式（如 192.168.001.001），而比对
     * 端 inet_ntop 只产出规范 192.168.1.1，非规范输入会静默 DENIED。写入前统一
     * 规范化为规范点分十进制，消除比对歧义。 */
    struct in_addr in4;
    if (inet_pton(AF_INET, src->addr, &in4) == 1) {
        inet_ntop(AF_INET, &in4, dst->addr, sizeof(dst->addr));
    } else {
        memcpy(dst->addr, src->addr, strlen(src->addr) + 1);
    }
    memcpy(dst->node_id, src->node_id, strlen(src->node_id) + 1);
    dst->state          = src->state;
    dst->boot_id        = src->boot_id;
    dst->applied_offset = src->applied_offset;
    dst->lag            = src->lag;
    return true;
}

/* 查 addr 在表 [0,n) 中的位置；未找到返回 -1。 */
static int peer_find(const rtdbd_peer_table_t *t, const char *addr, uint32_t n)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(t->peers[i].addr, addr) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int rtdbd_peer_table_set(rtdbd_peer_table_t *t, const rtdbd_peer_t *peers, uint32_t n,
                          const char *self_addr)
{
    if (!t || (!peers && n > 0) || n > RTDBD_PEER_MAX) {
        return -1; /* 超上限 / 参数非法，旧表不变 */
    }

    /* 临时表：全部校验通过后再交换（原子替换，失败旧表不变）。 */
    rtdbd_peer_table_t next;
    rtdbd_peer_table_init(&next);
    for (uint32_t i = 0; i < n; ++i) {
        rtdbd_peer_t normalized;
        if (!peer_copy_checked(&normalized, &peers[i], self_addr)) {
            return -1; /* 旧表保持不变 */
        }
        if (peer_find(&next, normalized.addr, next.count) >= 0) {
            continue; /* 重复 IP 去重（§8.3-6） */
        }
        if (next.count >= RTDBD_PEER_MAX) {
            return -1;
        }
        next.peers[next.count++] = normalized;
    }
    *t = next;
    return 0;
}

int rtdbd_peer_table_add(rtdbd_peer_table_t *t, const rtdbd_peer_t *p,
                          const char *self_addr)
{
    if (!t || !p) {
        return -1;
    }
    rtdbd_peer_t normalized;
    if (!peer_copy_checked(&normalized, p, self_addr)) {
        return -1;
    }
    if (peer_find(t, normalized.addr, t->count) >= 0) {
        return 1; /* 幂等：已存在 */
    }
    if (t->count >= RTDBD_PEER_MAX) {
        return -1; /* 表满 */
    }
    t->peers[t->count++] = normalized;
    return 0; /* 新增成功 → 上层入延迟动作队列触发 on_join 全量 */
}

int rtdbd_peer_table_del(rtdbd_peer_table_t *t, const char *addr)
{
    if (!t || !addr) {
        return -1;
    }
    int idx = peer_find(t, addr, t->count);
    if (idx < 0) {
        return 1; /* 不存在：无副作用 */
    }
    /* 末位交换法（与既有 UDS 订阅移除行为一致；成员表不保证顺序） */
    t->peers[(uint32_t)idx] = t->peers[t->count - 1];
    memset(&t->peers[t->count - 1], 0, sizeof(t->peers[0]));
    t->count--;
    return 0;
}

const rtdbd_peer_t *rtdbd_peer_table_get(const rtdbd_peer_table_t *t, const char *addr)
{
    if (!t || !addr) {
        return NULL;
    }
    int idx = peer_find(t, addr, t->count);
    return idx >= 0 ? &t->peers[(uint32_t)idx] : NULL;
}
