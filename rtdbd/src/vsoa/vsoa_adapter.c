/**
 * @file vsoa_adapter.c
 * @brief VSOA 前端适配层实现（M1.3b）：VSOA 帧 ⇄ transport 无关 dispatch 桥接。
 *
 * 关键约束（doc 13）：
 * - §7.2：status 用 0x80 + RTDBD_ST_*（rtdbd_st_to_vsoa_status）。
 * - §9.3：vsoa_payload_t.data 直放定长 POD（零序列化）；NOTIFY/SNAPSHOT_DONE 走
 *   cli_datagram 定向、禁用 publish；quick channel 禁承载增量/快照/ack/握手
 *   （本文件只用 RPC 通道，未使用 quick）。
 * - §8.3-3：命令回调在 vsoa_server_input_fds() 内被调用，故回调内不得阻塞、
 *   不得 reconnect（这类动作须走延迟动作队列）。
 */
#include "vsoa_adapter.h"

#include "dispatch/rtdbd_dispatch.h"
#include "cluster/rtdbd_peers_global.h"
#include "session/rtdbd_session.h" /* per-cli 传输无关会话（§8.3-3） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>
#include <vsoa_server.h>

/* 响应体上限：与既有 UDS 侧单帧负载量级一致（快照走分页 RPC，不在此限内）。 */
/* M3.3 切片 C：/sync/peers/get 回 4104B 定长 POD（rtdbd_peers_resp_t），
 * 原 4096B 装不下；扩容并留出余量。 */
#define RTDBD_VSOA_RESP_MAX 8192u

struct rtdbd_vsoa_adapter {
    vsoa_server_t     *server;
    rtdbd_vsoa_handler_t handler;
    uint16_t           port;
    /* 服务端能力配置快照（/caps）：与 UDS 侧共用同一 dispatch 实现，
     * 故两前端产出的 32B 响应体字节一致。 */
    rtdbd_dispatch_cfg_t cfg;
    bool                 cfg_set;
    /* 点位库句柄（读点能力用）：由 rtdbd 注入真实 handle，两前端读同一份数据。 */
    indurtdb_t          *handle;
    /* 写路径依赖（§8.3-4：与 UDS 同一鉴权函数、同一 deny-by-default）。 */
    const irt_policy_t  *policy;
    const rtdbd_conf_t  *conf;
    rtdbd_vsoa_audit_fn  audit;
    void                *audit_ctx;
    uint32_t             default_uid;
    bool                 allow_unauthenticated; /* 仅本机联调（默认关闭） */
    /* 预共享 token 表（§8.3-4：32 条上限、零动态分配） */
    rtdbd_vsoa_token_entry_t tokens[RTDBD_VSOA_MAX_TOKEN];
    /* per-cli 会话表（传输无关会话按连接隔离，§8.3-3/§8.3-4）：
     * partition 等连接级状态挂在这里；连接建立/断开由 on_cli 回调维护。 */
    struct {
        vsoa_cli_id_t   id;
        bool            used;
        rtdbd_session_t session;
    } cli[RTDBD_VSOA_MAX_CLI];
    /* 回调内使用的响应缓冲：SDK 规定 payload 在回调返回后失效，故必须自备。 */
    uint8_t            resp[RTDBD_VSOA_RESP_MAX];
};

/* 13 个镜像能力（doc 13 §7.1）；注册时补版本段前缀。 */
static const char *const VSOA_MIRROR_URLS[] = {
    "/ping", "/point/write", "/point/batch_write", "/point/get", "/point/batch_read",
    "/subscription/add", "/subscription/remove", "/subscription/set",
    "/notify", "/snapshot_done", "/caps", "/select", "/health",
};
#define VSOA_MIRROR_COUNT (sizeof(VSOA_MIRROR_URLS) / sizeof(VSOA_MIRROR_URLS[0]))

/* per-cli 会话查找（线性扫描：连接数上限 32，且只在命令路径上调用）。 */
static rtdbd_session_t *cli_session(rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id)
{
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used && a->cli[i].id == id) {
            return &a->cli[i].session;
        }
    }
    return NULL;
}

/* 连接建立/断开回调：维护 per-cli 会话（§8.3-3 传输无关会话 + §8.3-4 身份绑定）。
 * 断开时释放会话槽，避免连接 id 复用导致状态串台。 */
static void vsoa_on_cli(void *arg, vsoa_server_t *server, vsoa_cli_id_t id, bool connect)
{
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)arg;
    (void)server;
    if (!a) {
        return;
    }
    if (connect) {
        for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
            if (a->cli[i].used) {
                continue;
            }
            if (!rtdbd_session_init(&a->cli[i].session, RTDBD_SUBTAB_MIN_CAP)) {
                return; /* 订阅表初始分配失败：连接仍可建立，但无会话状态 */
            }
            a->cli[i].id = id;
            a->cli[i].used = true;
            a->cli[i].session.cred.uid = a->default_uid;
            a->cli[i].session.cred.cred_ok = (a->default_uid != 0);
            return;
        }
        return; /* 会话表满：不覆盖既有会话 */
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used && a->cli[i].id == id) {
            rtdbd_session_free(&a->cli[i].session);
            a->cli[i].used = false;
            a->cli[i].id = 0;
            return;
        }
    }
}

/* hello 握手处理（§8.3-4）：请求体带预共享 token + service_id；服务端按定长
 * token 表映射 (uid, partition, service_id, role) 写入该连接的 rtdbd_session_t，
 * 并置 cli_set_authed（挡 publish 订阅）。
 * ⚠ token 明文比对：token 属链路准入凭据，线上应由 TLS/专线承载（§8.3-4 的
 * vsoa_server_passwd 只作链路准入，不承担按点鉴权）。 */
static uint16_t vsoa_hello(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                            vsoa_cli_id_t id, const uint8_t *req, size_t req_len)
{
    if (req_len != sizeof(rtdbd_vsoa_hello_req_t)) {
        return RTDBD_ST_BAD_REQUEST;
    }
    rtdbd_vsoa_hello_req_t hello;
    memcpy(&hello, req, sizeof(hello));
    hello.token[sizeof(hello.token) - 1] = '\0';
    hello.service_id[sizeof(hello.service_id) - 1] = '\0';

    const rtdbd_vsoa_token_entry_t *ent = NULL;
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used && strcmp(a->tokens[i].token, hello.token) == 0) {
            ent = &a->tokens[i];
            break;
        }
    }
    if (!ent) {
        return RTDBD_ST_DENIED; /* token 不匹配 */
    }

    /* 来源强校验（§8.3-4）：plc 必须来自 loopback；sync_peer 必须来自
     * sync.peers（peer 表尚未实现 → 显式拒绝，不放行）。 */
    struct sockaddr_storage peer_addr;
    socklen_t plen = sizeof(peer_addr);
    memset(&peer_addr, 0, sizeof(peer_addr));
    if (!vsoa_server_cli_address(server, id, (struct sockaddr *)&peer_addr, &plen)) {
        return RTDBD_ST_DENIED;
    }
    if (ent->role == RTDBD_ROLE_PLC || ent->role == RTDBD_ROLE_SYNC_ADMIN) {
        /* §197：sync_admin 与 plc 一样只接受 loopback（成员管控不得跨机）。 */
        if (peer_addr.ss_family != AF_INET) {
            return RTDBD_ST_DENIED;
        }
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer_addr;
        if ((ntohl(in4->sin_addr.s_addr) & 0xFF000000u) != 0x7F000000u) {
            return RTDBD_ST_DENIED; /* plc / sync_admin 必须 loopback */
        }
    } else {
        return RTDBD_ST_NOT_IMPLEMENTED; /* sync_peer 来源校验依赖成员表（未实现） */
    }

    rtdbd_session_t *sess = cli_session(a, id);
    if (!sess) {
        return RTDBD_ST_NOT_IMPLEMENTED; /* 无会话状态 */
    }
    /* 请求携带的 service_id 必须与 token 表登记一致（防身份混淆） */
    if (strcmp(hello.service_id, ent->service_id) != 0) {
        return RTDBD_ST_DENIED;
    }
    sess->cred.uid = ent->uid;
    sess->cred.cred_ok = true;
    sess->role = ent->role;
    sess->partition = ent->partition;
    (void)rtdbd_session_set_service_id(sess, ent->service_id);
    /* 握手成功才置 authed（挡 publish 订阅，§8.3-4） */
    (void)vsoa_server_cli_set_authed(server, id, true);
    return RTDBD_ST_OK;
}

/* 变更通知定向投递（§8.3-5）：对每个**订阅了该点**的 per-cli 会话做 RBE 门控，
 * 通过者用 cli_datagram 逐连接定向发送（**禁用** publish / quick_datagram）。
 * role=sync_peer 的会话不得接收 NOTIFY（防回环，§8.3-4/§8.3-5）。 */
static void vsoa_notify_dispatch(rtdbd_vsoa_adapter_t *a,
                                 const rtdbd_write_req_t *w,
                                 const indurtdb_meta_t *meta)
{
    if (!a || !a->server || !w) {
        return;
    }
    rtdbd_notify_t nt;
    if (rtdbd_dispatch_notify(a->handle, w, &nt) != RTDBD_ST_OK) {
        return;
    }
    vsoa_url_t url;
    memset(&url, 0, sizeof(url));
    static const char notify_url[] = RTDBD_VSOA_URL_PREFIX "/notify";
    url.url = (char *)notify_url;
    url.url_len = sizeof(notify_url) - 1;

    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (!a->cli[i].used) {
            continue;
        }
        rtdbd_session_t *sess = &a->cli[i].session;
        if (rtdbd_session_is_sync_peer(sess)) {
            continue; /* sync_peer 不得收 NOTIFY（防回环） */
        }
        if (!rtdbd_session_subs_contains(sess, w->point_id)) {
            continue; /* 未订阅该点 */
        }
        /* RBE 门控：死区未越过则抑制（共享语义，与 UDS 同一状态机） */
        if (!rtdbd_dispatch_notify_rbe(&sess->sub, w, meta)) {
            continue;
        }
        vsoa_payload_t pl;
        memset(&pl, 0, sizeof(pl));
        pl.data = &nt;
        pl.data_len = sizeof(nt);
        (void)vsoa_server_cli_datagram(a->server, a->cli[i].id, &url, &pl);
    }
}

/* 命令回调：VSOA URL + payload → 归一化 URL → opcode → dispatch → reply。 */
/* ---- v3.12 M3.3 切片 C：/sync/peers/{set,get}（§197）----
 * 限 role=sync_admin + 已握手 + loopback；整组替换与 UDS **共用**
 * rtdbd_peers_apply_set()，故两前端看到同一张表、同一套延迟动作。 */
static bool vsoa_cli_loopback(vsoa_server_t *server, vsoa_cli_id_t id)
{
    struct sockaddr_storage sa;
    socklen_t slen = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (!vsoa_server_cli_address(server, id, (struct sockaddr *)&sa, &slen) ||
        sa.ss_family != AF_INET) {
        return false;
    }
    const struct sockaddr_in *in4 = (const struct sockaddr_in *)&sa;
    return (ntohl(in4->sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u;
}

static uint16_t vsoa_peers_admin(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                                 vsoa_cli_id_t id, const char *bare,
                                 const uint8_t *req, size_t req_len,
                                 size_t *resp_len)
{
    *resp_len = 0;
    rtdbd_session_t *sess = cli_session(a, id);
    if (!sess || !sess->cred.cred_ok) return RTDBD_ST_DENIED; /* 未握手 */
    if (sess->role != RTDBD_ROLE_SYNC_ADMIN) return RTDBD_ST_DENIED;
    if (!vsoa_cli_loopback(server, id)) return RTDBD_ST_DENIED;

    if (strcmp(bare, "/sync/peers/set") == 0) {
        if (req_len != sizeof(rtdbd_peers_resp_t)) return RTDBD_ST_BAD_REQUEST;
        rtdbd_peers_resp_t body;
        memcpy(&body, req, sizeof(body));
        if (body.count > RTDBD_PEER_MAX) return RTDBD_ST_BAD_REQUEST;
        /* 与 UDS op_set_peers 同一函数：原子整组替换、失败旧表不变、diff 入队 */
        if (rtdbd_peers_apply_set(body.peers, body.count) != 0) return RTDBD_ST_BAD_REQUEST;
        return RTDBD_ST_OK;
    }

    rtdbd_peers_resp_t out;
    memset(&out, 0, sizeof(out));
    rtdbd_peer_table_t *tbl = rtdbd_peers_global();
    out.count = tbl->count;
    if (out.count > 0u) {
        memcpy(out.peers, tbl->peers, (size_t)out.count * sizeof(rtdbd_peer_t));
    }
    memcpy(a->resp, &out, sizeof(out));
    *resp_len = sizeof(out);
    return RTDBD_ST_OK;
}

static void vsoa_on_cmd(void *arg, vsoa_server_t *server, vsoa_cli_id_t id,
                         vsoa_header_t *hdr, vsoa_url_t *url, vsoa_payload_t *payload)
{
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)arg;
    if (!a || !url || !url->url) {
        return;
    }

    /* URL 非 NUL 结尾（只有 url_len），须本地拷贝后归一化。 */
    char raw[256];
    size_t n = url->url_len;
    if (n >= sizeof(raw)) {
        n = sizeof(raw) - 1;
    }
    memcpy(raw, url->url, n);
    raw[n] = '\0';

    char bare[128];
    if (!rtdbd_normalize_url(raw, bare, sizeof(bare))) {
        return; /* 缓冲不足：无法归一化，直接不回（由客户端超时暴露） */
    }

    const uint8_t *req = NULL;
    size_t req_len = 0;
    if (payload && payload->data && payload->data_len > 0) {
        req = (const uint8_t *)payload->data;
        req_len = payload->data_len;
    }

    /* hello 是 VSOA 层专有 URL（§8.3-4），不属于 rtdbd opcode 表 → 先处理 */
    if (strcmp(bare, "/hello") == 0) {
        uint16_t hst = vsoa_hello(a, server, id, req, req_len);
        vsoa_payload_t hout;
        memset(&hout, 0, sizeof(hout));
        uint32_t hseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t htun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)hst),
                                    hseq, htun, &hout);
        return;
    }

    /* /sync/peers/{set,get} 是 VSOA 专有管控 URL（§197），不进镜像 opcode 表。 */
    if (strcmp(bare, "/sync/peers/set") == 0 || strcmp(bare, "/sync/peers/get") == 0) {
        size_t admin_len = 0;
        uint16_t ast = vsoa_peers_admin(a, server, id, bare, req, req_len, &admin_len);
        vsoa_payload_t aout;
        memset(&aout, 0, sizeof(aout));
        if (admin_len > 0 && admin_len <= sizeof(a->resp)) {
            aout.data = a->resp;
            aout.data_len = admin_len;
        }
        uint32_t aseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t atun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)ast),
                                    aseq, atun, &aout);
        return;
    }

    uint16_t opcode = 0;
    if (rtdbd_opcode_from_url(bare, &opcode) != 0) {
        return; /* 非镜像能力：不处理（也不落错误 reply，避免半实现误导） */
    }

    size_t resp_len = 0;
    int st = 0;
    if (opcode == RTDBD_OP_CAPS) {
        /* /caps：走**共享**的 transport 无关语义（与 UDS 侧同一函数），
         * 故 VSOA 与 UDS 两前端的 32B 响应体字节一致（§8.3-3 / §7.4）。 */
        if (!a->cfg_set) {
            st = 9; /* RTDBD_ST_NOT_IMPLEMENTED：未配置能力快照，显式未实现 */
        } else {
            st = (int)rtdbd_dispatch_caps(&a->cfg, a->resp, sizeof(a->resp), &resp_len);
        }
    } else if (opcode == RTDBD_OP_GET) {
        /* /point/get：读点值 —— 走**共享**的 transport 无关语义（与 UDS 侧同一
         * 函数），故两前端读同一点位得到字节一致的 64B 响应体。 */
        if (!a->handle) {
            st = 9; /* RTDBD_ST_NOT_IMPLEMENTED：未注入点位库句柄 */
        } else if (req_len != sizeof(rtdbd_get_req_t)) {
            st = 1; /* RTDBD_ST_BAD_REQUEST：请求体长度不符 */
        } else {
            rtdbd_get_req_t greq;
            memcpy(&greq, req, sizeof(greq));
            st = (int)rtdbd_dispatch_get(a->handle, greq.point_id,
                                         a->resp, sizeof(a->resp), &resp_len);
        }
    } else if (opcode == RTDBD_OP_SELECT) {
        /* /select：设置**连接级**默认分区（写入该连接的 session）。
         * 语义与 UDS 侧 op_select 一致：分区非法 → DENIED 且**不改**连接状态。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9; /* 无会话状态（未连接或会话表满） */
        } else if (req_len != sizeof(uint32_t)) {
            st = 1; /* BAD_REQUEST：请求体长度不符 */
        } else {
            uint32_t part = 0;
            memcpy(&part, req, sizeof(part));
            if (a->conf && !rtdbd_conf_partition_allowed(a->conf, part)) {
                st = 2; /* DENIED：不改连接状态 */
            } else {
                sess->partition = part;
                st = 0;
            }
        }
    } else if (opcode == RTDBD_OP_SUBSCRIBE || opcode == RTDBD_OP_UNSUBSCRIBE) {
        /* 订阅 / 退订：会话的订阅集合是**传输无关真源**（session.sub），
         * VSOA 重连后据此重建（§8.3-5）。鉴权走共享语义（点位分区）。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9; /* 无会话状态 */
        } else if (!sess->cred.cred_ok && !a->allow_unauthenticated) {
            st = 2; /* DENIED：未握手（§8.3-4 强制 hello 握手） */
        } else if (req_len != sizeof(rtdbd_sub_req_t)) {
            st = 1; /* BAD_REQUEST */
        } else {
            rtdbd_sub_req_t sreq;
            memcpy(&sreq, req, sizeof(sreq));
            if (opcode == RTDBD_OP_SUBSCRIBE) {
                uint16_t authz = rtdbd_dispatch_subscribe_authz(a->handle, a->policy,
                                                                a->conf, sess->cred.uid,
                                                                sreq.point_id);
                if (authz != RTDBD_ST_OK) {
                    st = authz;
                } else {
                    rtdbd_subs_add_result_t ar = rtdbd_session_subs_add(sess, sreq.point_id,
                                                                        RTDBD_SUB_MAX);
                    st = (ar == RTDBD_SUBS_ADD_FAIL) ? 5 /* NO_MEM */ : 0;
                }
            } else {
                rtdbd_session_subs_remove(sess, sreq.point_id);
                st = 0;
            }
        }
    } else if (opcode == RTDBD_OP_WRITE) {
        /* /point/write：写权威路径。鉴权与落值走**共享**语义
         * （rtdbd_dispatch_write），审计由前端回调完成。
         * ⚠ 变更通知（定向投递）尚未实现 —— 见 14 号计划 M1.8 留白。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9;
        } else if (!sess->cred.cred_ok && !a->allow_unauthenticated) {
            st = 2; /* DENIED：未握手（§8.3-4 强制 hello 握手） */
        } else if (!a->policy || !a->conf || !a->handle) {
            st = 9; /* 未注入写路径依赖：显式未实现，绝不默认放行 */
        } else if (req_len != sizeof(rtdbd_write_req_t)) {
            st = 1; /* BAD_REQUEST */
        } else {
            rtdbd_write_req_t wreq;
            memcpy(&wreq, req, sizeof(wreq));
            indurtdb_meta_t wmeta;
            st = (int)rtdbd_dispatch_write(a->handle, a->policy, a->conf, &wreq,
                                           sess->cred.uid, sess->partition, &wmeta);
            if (st == RTDBD_ST_OK) {
                if (a->audit) {
                    a->audit(a->audit_ctx, sess->cred.pid, sess->cred.uid,
                             sess->partition, wreq.point_id, 0 /* ts_ns 由前端补 */,
                             sess->service_id);
                }
                /* 变更通知：逐连接**定向**投递（§8.3-5，禁用 publish / quick）。
                 * 帧内容与 RBE 门控走共享语义（两端一致），投递机制属传输层。 */
                vsoa_notify_dispatch(a, &wreq, &wmeta);
            }
        }
    } else if (a->handler.handle) {
        st = a->handler.handle(a->handler.ctx, opcode, req, req_len,
                               a->resp, sizeof(a->resp), &resp_len);
    }

    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    if (resp_len > 0 && resp_len <= sizeof(a->resp)) {
        out.data = a->resp;
        out.data_len = resp_len;
    }
    /* 字节序：cmd 回调的 vsoa_header_t 多字节成员是**网络字节序**
     * （SDK 头文件明示），而 vsoa_server_cli_reply 的 seqno/tunid 期望**主机
     * 字节序**（SDK 内部再转网络序）。不转换时：同一连接的第 1 个请求
     * （seqno=0）恰好正常，第 2 个起（seqno≠0）客户端匹配不上 → RPC 超时。
     * 该缺陷只有在同一连接连续发第二个 RPC 时才暴露。 */
    uint32_t seqno = hdr ? ntohl(hdr->seqno) : 0u;
    uint16_t tunid = hdr ? ntohs(hdr->tunid) : 0u;
    /* status 走 0x80 + RTDBD_ST_*（§7.2）；uint8_t 足够（0x80..0x89）。 */
    (void)vsoa_server_cli_reply(server, id, (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)st),
                                seqno, tunid, &out);
}

rtdbd_vsoa_adapter_t *rtdbd_vsoa_adapter_create(const rtdbd_vsoa_handler_t *handler)
{
    if (!handler || !handler->handle) {
        return NULL;
    }
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->handler = *handler;
    a->server = vsoa_server_create("{\"name\":\"indurtdb_rtdbd\",\"n\":1}");
    if (!a->server) {
        free(a);
        return NULL;
    }
    vsoa_server_on_cli(a->server, vsoa_on_cli, a);
    /* hello 是 VSOA 层专有 URL（§8.3-4），不属于 rtdbd 镜像清单，须单独注册
     * listener，否则 SDK 会以「无对应 URL」回 status=3（实测）。 */
    {
        static char hello_full[128];
        int hk = snprintf(hello_full, sizeof(hello_full), "%s/hello",
                          RTDBD_VSOA_URL_PREFIX);
        if (hk > 0 && (size_t)hk < sizeof(hello_full)) {
            vsoa_url_t hu;
            memset(&hu, 0, sizeof(hu));
            hu.url = hello_full;
            hu.url_len = strlen(hello_full);
            if (!vsoa_server_add_listener(a->server, &hu, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server);
                free(a);
                return NULL;
            }
        }
    }
    {
        static const char *const admin_urls[] = {"/sync/peers/set", "/sync/peers/get"};
        static char admin_full[2][128];
        for (size_t i = 0; i < sizeof(admin_urls) / sizeof(admin_urls[0]); ++i) {
            int ak = snprintf(admin_full[i], sizeof(admin_full[i]), "%s%s",
                              RTDBD_VSOA_URL_PREFIX, admin_urls[i]);
            if (ak <= 0 || (size_t)ak >= sizeof(admin_full[i])) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
            vsoa_url_t au;
            memset(&au, 0, sizeof(au));
            au.url = admin_full[i];
            au.url_len = strlen(admin_full[i]);
            if (!vsoa_server_add_listener(a->server, &au, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
        }
    }
    for (size_t i = 0; i < VSOA_MIRROR_COUNT; ++i) {
        char full[128];
        int k = snprintf(full, sizeof(full), "%s%s", RTDBD_VSOA_URL_PREFIX,
                         VSOA_MIRROR_URLS[i]);
        if (k <= 0 || (size_t)k >= sizeof(full)) {
            continue;
        }
        vsoa_url_t u;
        memset(&u, 0, sizeof(u));
        u.url = full;
        u.url_len = strlen(full);
        if (!vsoa_server_add_listener(a->server, &u, vsoa_on_cmd, a)) {
            /* 注册失败即视为不可用：宁可不启动，也不做半实现。 */
            vsoa_server_close(a->server);
            free(a);
            return NULL;
        }
    }
    return a;
}

void rtdbd_vsoa_adapter_destroy(rtdbd_vsoa_adapter_t *a)
{
    if (!a) {
        return;
    }
    if (a->server) {
        vsoa_server_close(a->server);
    }
    free(a);
}

bool rtdbd_vsoa_adapter_start(rtdbd_vsoa_adapter_t *a, uint16_t port, uint16_t *bound_port)
{
    if (!a || !a->server) {
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 现场主写入口为 VSOA loopback */

    if (!vsoa_server_start(a->server, (const struct sockaddr *)&addr, sizeof(addr))) {
        return false;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    memset(&bound, 0, sizeof(bound));
    if (!vsoa_server_address(a->server, (struct sockaddr *)&bound, &blen)) {
        return false;
    }
    a->port = ntohs(bound.sin_port);
    if (bound_port) {
        *bound_port = a->port;
    }
    return true;
}

int rtdbd_vsoa_adapter_fds(rtdbd_vsoa_adapter_t *a, void *rfds)
{
    if (!a || !a->server || !rfds) {
        return -1;
    }
    return vsoa_server_fds(a->server, (fd_set *)rfds);
}

void rtdbd_vsoa_adapter_input(rtdbd_vsoa_adapter_t *a, const void *rfds)
{
    if (!a || !a->server) {
        return;
    }
    vsoa_server_input_fds(a->server, (const fd_set *)rfds);
}

uint16_t rtdbd_vsoa_adapter_port(const rtdbd_vsoa_adapter_t *a)
{
    return a ? a->port : 0u;
}

void rtdbd_vsoa_adapter_set_cfg(rtdbd_vsoa_adapter_t *a, const rtdbd_dispatch_cfg_t *cfg)
{
    if (!a || !cfg) {
        return;
    }
    a->cfg = *cfg;
    a->cfg_set = true;
}

void rtdbd_vsoa_adapter_set_handle(rtdbd_vsoa_adapter_t *a, indurtdb_t *h)
{
    if (!a) {
        return;
    }
    a->handle = h;
}

void rtdbd_vsoa_adapter_set_policy(rtdbd_vsoa_adapter_t *a, const irt_policy_t *policy)
{
    if (a) {
        a->policy = policy;
    }
}

void rtdbd_vsoa_adapter_set_conf(rtdbd_vsoa_adapter_t *a, const rtdbd_conf_t *conf)
{
    if (a) {
        a->conf = conf;
    }
}

void rtdbd_vsoa_adapter_set_audit(rtdbd_vsoa_adapter_t *a, rtdbd_vsoa_audit_fn fn, void *ctx)
{
    if (!a) {
        return;
    }
    a->audit = fn;
    a->audit_ctx = ctx;
}

void rtdbd_vsoa_adapter_set_allow_unauthenticated(rtdbd_vsoa_adapter_t *a, bool allow)
{
    if (a) {
        a->allow_unauthenticated = allow;
    }
}

int rtdbd_vsoa_adapter_add_token(rtdbd_vsoa_adapter_t *a, const char *token,
                                  uint32_t uid, uint32_t partition,
                                  const char *service_id, rtdbd_session_role_t role)
{
    if (!a || !token || token[0] == '\0' || !service_id) {
        return -2;
    }
    if (strlen(token) >= sizeof(a->tokens[0].token) ||
        strlen(service_id) >= sizeof(a->tokens[0].service_id)) {
        return -2;
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used && strcmp(a->tokens[i].token, token) == 0) {
            /* 同一 token 重复登记 → 更新该条目（不新增） */
            a->tokens[i].uid = uid;
            a->tokens[i].partition = partition;
            snprintf(a->tokens[i].service_id, sizeof(a->tokens[i].service_id), "%s", service_id);
            a->tokens[i].role = role;
            return 0;
        }
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used) {
            continue;
        }
        a->tokens[i].used = true;
        snprintf(a->tokens[i].token, sizeof(a->tokens[i].token), "%s", token);
        a->tokens[i].uid = uid;
        a->tokens[i].partition = partition;
        snprintf(a->tokens[i].service_id, sizeof(a->tokens[i].service_id), "%s", service_id);
        a->tokens[i].role = role;
        return 0;
    }
    return -1; /* 表满 */
}

void rtdbd_vsoa_adapter_set_default_uid(rtdbd_vsoa_adapter_t *a, uint32_t uid)
{
    if (!a) {
        return;
    }
    a->default_uid = uid;
    /* 已建立连接的会话同步生效（避免连上后改配置不生效） */
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used) {
            a->cli[i].session.cred.uid = uid;
            a->cli[i].session.cred.cred_ok = (uid != 0);
        }
    }
}
