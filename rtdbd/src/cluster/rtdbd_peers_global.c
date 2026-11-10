/**
 * @file rtdbd_peers_global.c
 * @brief v3.12 M3.3：成员表进程级单例 + 本节点地址（两前端共用）。
 *
 * 静态存储期零初始化等价 rtdbd_peer_table_init()；主循环单线程，零锁。
 */
#include "rtdbd_peers_global.h"

#include <string.h>

static rtdbd_peer_table_t g_peers_global;
static char g_self[RTDBD_PEER_ADDR_MAX] = {0};

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
