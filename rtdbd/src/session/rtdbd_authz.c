/**
 * @file rtdbd_authz.c
 * @brief §8.3-4 身份/来源授权判据的纯逻辑实现（零传输依赖，见 .h）。
 */
#include "session/rtdbd_authz.h"

#include <rtdbd/protocol.h> /* RTDBD_ST_* */

#include <string.h>

/* 127.0.0.0/8（主机序）：与 §8.3-4 对 plc / sync_admin 的 loopback 约束一致。 */
#define RTDBD_AUTHZ_LOOPBACK_CLASS_A 0x7F000000u
#define RTDBD_AUTHZ_LOOPBACK_MASK    0xFF000000u

uint16_t rtdbd_authz_source_allowed(rtdbd_session_role_t role,
                                     bool is_ipv4, uint32_t addr_host)
{
    switch (role) {
    case RTDBD_ROLE_PLC:
    case RTDBD_ROLE_SYNC_ADMIN:
        if (!is_ipv4) {
            return RTDBD_ST_DENIED;
        }
        return ((addr_host & RTDBD_AUTHZ_LOOPBACK_MASK) == RTDBD_AUTHZ_LOOPBACK_CLASS_A)
                   ? RTDBD_ST_OK
                   : RTDBD_ST_DENIED;
    case RTDBD_ROLE_SYNC_PEER:
        /* 不可达占位：vsoa_adapter 已对 SYNC_PEER 特判并直接调用
         * rtdbd_authz_sync_peer_source_allowed()（需运行时成员表），本分支
         * 实际不会被调用；保留为「无成员表上下文」显式 deny 兜底，绝不默认放行。 */
        return RTDBD_ST_NOT_IMPLEMENTED;
    case RTDBD_ROLE_UNKNOWN:
    default:
        return RTDBD_ST_DENIED;
    }
}

bool rtdbd_authz_url_allowed(rtdbd_session_role_t role, const char *bare_url)
{
    if (!bare_url || bare_url[0] == '\0') {
        return false;
    }
    if (strcmp(bare_url, "/hello") == 0) {
        return true; /* 握手入口：任何角色都须先能握手 */
    }

    const bool is_sync = (strncmp(bare_url, "/sync/", 6) == 0);

    switch (role) {
    case RTDBD_ROLE_PLC:
        return !is_sync;

    case RTDBD_ROLE_SYNC_ADMIN:
        return strcmp(bare_url, "/sync/peers/set") == 0 ||
               strcmp(bare_url, "/sync/peers/get") == 0;

    case RTDBD_ROLE_SYNC_PEER:
        return is_sync;

    case RTDBD_ROLE_UNKNOWN:
    default:
        return false; /* deny-by-default */
    }
}

uint16_t rtdbd_authz_sync_peer_source_allowed(const rtdbd_peer_table_t *tbl,
                                              const char *addr_str)
{
    if (!tbl || !rtdbd_peer_addr_valid(addr_str)) {
        return RTDBD_ST_DENIED;
    }
    /* §8.3-4：sync_peer 必须是已知集群成员（来源地址落在运行时成员表 addr 集合），
     * 防止任意主机伪装成同步对等节点。 */
    return (rtdbd_peer_table_get(tbl, addr_str) != NULL) ? RTDBD_ST_OK
                                                         : RTDBD_ST_DENIED;
}
