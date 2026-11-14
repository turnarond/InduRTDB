/**
 * @file rtdbd_peers_global.c
 * @brief v3.12 M3.3：成员表进程级单例 + 本节点地址（两前端共用）。
 *
 * 静态存储期零初始化等价 rtdbd_peer_table_init()；主循环单线程，零锁。
 */
#include "rtdbd_peers_global.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static rtdbd_peer_table_t g_peers_global;
static char g_self[RTDBD_PEER_ADDR_MAX] = {0};
static uint64_t g_boot_id = 0;
static uint64_t g_local_offset = 0;

rtdbd_peer_table_t *rtdbd_peers_global(void)
{
    return &g_peers_global;
}

const char *rtdbd_peers_self_addr(void)
{
    return g_self;
}

bool rtdbd_peers_set_self_addr(const char *addr)
{
    /* 非法 / 超长一律拒绝且**不改写**旧值（拒绝自地址是安全边界，不能半写）。 */
    if (!addr || !rtdbd_peer_addr_valid(addr)) {
        return false;
    }
    size_t n = strlen(addr);
    if (n == 0u || n >= sizeof(g_self)) {
        return false;
    }
    memcpy(g_self, addr, n + 1u);
    return true;
}

/* 本节点启动标识（IR6 proc_restart_no_fullsync：boot_id 变化即「非首次」启动）。 */
uint64_t rtdbd_peers_boot_id(void)
{
    return g_boot_id;
}

void rtdbd_peers_set_boot_id(uint64_t id)
{
    g_boot_id = id;
}

/* 生成启动标识：CLOCK_MONOTONIC 纳秒高位 + 随机低位（每次启动高概率不同）。
 * ⚠ 仅用于 IR6「是否首次启动」判定，非密码学随机。 */
uint64_t rtdbd_boot_id_generate(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 1u;
    }
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned)(ts.tv_sec ^ ts.tv_nsec));
        seeded = true;
    }
    uint64_t t = ((uint64_t)ts.tv_sec << 32) | ((uint64_t)ts.tv_nsec & 0xFFFFFFFFu);
    uint64_t r = (uint64_t)rand();
    return t ^ (r << 16);
}

/* 本节点已应用偏移参考（最小 lag 计算用；同步流应用 delta 时推进，默认 0）。 */
uint64_t rtdbd_peers_local_offset(void)
{
    return g_local_offset;
}

void rtdbd_peers_set_local_offset(uint64_t off)
{
    g_local_offset = off;
}
