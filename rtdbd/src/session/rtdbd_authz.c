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
        /* 成员表来源校验尚未实现：显式未实现，绝不默认放行。 */
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
