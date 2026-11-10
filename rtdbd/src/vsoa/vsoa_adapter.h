/**
 * @file vsoa_adapter.h
 * @brief VSOA 前端适配层（M1.3b）
 *
 * 本文件所在实现是**唯一**允许引用 vsoa_* 符号的文件：
 * - scripts/check_arch_layers.py：vsoa 属独立 ring，仅 server 层（rtdbd/、client/）
 *   可依赖；本文件在 rtdbd/ 内，合规。
 * - scripts/check_dispatch_boundary.py：dispatch 层（rtdbd/src/dispatch/）禁止出现
 *   vsoa_* 符号；本文件**不在** dispatch glob 内，故 dispatch 层保持零 vsoa 依赖，
 *   而 VSOA 专有符号集中在此。
 *
 * 职责（doc 13 §7.3 / §9.3）：VSOA 帧 ⇄ transport 无关 dispatch 的桥接。
 * - 入：VSOA URL（带版本段，如 "/indurtdb/v1/ping"）+ payload.data（定长 POD，零序列化）
 * - 出：status = 0x80 + RTDBD_ST_*（rtdbd_st_to_vsoa_status）+ payload.data
 * 业务语义由注入的 handler 回调提供；adapter 不认识 rtdbd_ctx_t，保持解耦，
 * 便于单测与 UDS 侧复用同一套 dispatch。
 */
#ifndef RTDBD_VSOA_ADAPTER_H_
#define RTDBD_VSOA_ADAPTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dispatch/rtdbd_dispatch.h"     /* rtdbd_dispatch_cfg_t（业务语义配置快照） */
#include "session/rtdbd_session.h"      /* rtdbd_session_role_t（角色枚举，§8.3-4） */

#ifdef __cplusplus
extern "C" {
#endif

/* VSOA URL 的版本段前缀（doc 13 §95「URL 带版本段」）。 */
#define RTDBD_VSOA_URL_PREFIX "/indurtdb/v1"

/* 业务处理后端：由上层（rtdbd 主循环）注入。
 * handle 返回 0 成功；非 0 视为 rtdbd 状态码（RTDBD_ST_*）。
 * resp/resp_len 用于回填响应体（定长 POD，零序列化）。 */
typedef struct {
    int (*handle)(void *ctx, uint16_t opcode,
                  const uint8_t *req, size_t req_len,
                  uint8_t *resp, size_t resp_cap, size_t *resp_len);
    void *ctx;
} rtdbd_vsoa_handler_t;

typedef struct rtdbd_vsoa_adapter rtdbd_vsoa_adapter_t;

/* 创建（内部创建 VSOA server 并注册 13 个镜像能力的 RPC 监听）。 */
rtdbd_vsoa_adapter_t *rtdbd_vsoa_adapter_create(const rtdbd_vsoa_handler_t *handler);

/* 销毁（关闭 server 并释放）。 */
void rtdbd_vsoa_adapter_destroy(rtdbd_vsoa_adapter_t *a);

/* 启动并绑定 loopback 端口；port=0 表示由系统分配。成功返回 true，
 * bound_port 输出实际端口（供测试与运维定位）。 */
bool rtdbd_vsoa_adapter_start(rtdbd_vsoa_adapter_t *a, uint16_t port, uint16_t *bound_port);

/* 供主循环合并（§8.3-3 单线程统一 poll）：把 VSOA 读 fd 填入 fd_set 并驱动
 * 事件循环——所有回调（含命令处理）都在 input_fds 中被调用，故业务回调不得
 * 在此阻塞，也不得在回调里 reconnect（改走延迟动作队列）。 */
int  rtdbd_vsoa_adapter_fds(rtdbd_vsoa_adapter_t *a, void *rfds /* fd_set* */);
void rtdbd_vsoa_adapter_input(rtdbd_vsoa_adapter_t *a, const void *rfds /* fd_set* */);

/* 已绑定端口（未启动返回 0）。 */
uint16_t rtdbd_vsoa_adapter_port(const rtdbd_vsoa_adapter_t *a);

/* 设置服务端能力配置快照（/caps 用）。由上层（rtdbd 主循环）从运行配置填充。
 * 未设置时 /caps 回 NOT_IMPLEMENTED —— 宁可显式未实现，也不用零值糊弄对端。 */
void rtdbd_vsoa_adapter_set_cfg(rtdbd_vsoa_adapter_t *a, const rtdbd_dispatch_cfg_t *cfg);

/* 注入点位库句柄（/point/get 等读点能力用）。由 rtdbd 主循环注入真实 handle，
 * 使 VSOA 前端与 UDS 前端读**同一份**点位数据（写权威路径唯一，§8.3-1）。
 * 未注入时读点能力回 NOT_IMPLEMENTED —— 显式未实现。 */
void rtdbd_vsoa_adapter_set_handle(rtdbd_vsoa_adapter_t *a, indurtdb_t *h);

/* 审计回调（前端特有副作用）：写成功后由 adapter 调用，参数与
 * irt_audit_record 对齐（pid / uid / partition / point_id / ts_ns），
 * 并按 §8.3-4 额外带 **service_id**（VSOA 侧的服务身份；UDS 侧为空）。 */
typedef void (*rtdbd_vsoa_audit_fn)(void *ctx, uint32_t pid, uint32_t uid,
                                    uint32_t partition, uint32_t point_id,
                                    uint64_t ts_ns, const char *service_id);

/* ---- hello 握手与预共享 token 表（§8.3-4）----
 *
 * 建连后客户端须先调 "/indurtdb/v1/hello"（携带预共享 token + service_id），
 * 服务端按**定长** token 表映射 (uid, partition, service_id, role) 写入该连接的
 * rtdbd_session_t；握手成功才置 cli_set_authed（挡 publish 订阅）。
 * token 表 32 条上限、零动态分配。
 *
 * hello 请求体（定长 POD，零序列化；token 与 service_id 均以 '\0' 结尾）：
 */
typedef struct {
    char token[64];
    char service_id[64];
} rtdbd_vsoa_hello_req_t;

/* token 表条目（定长，零动态分配）。 */
typedef struct {
    bool     used;
    char     token[64];        /* 预共享 token（明文比对；线上应由链路加密承载） */
    uint32_t uid;
    uint32_t partition;        /* 该身份的连接级默认分区 */
    char     service_id[64];
    rtdbd_session_role_t role; /* plc / sync_peer */
} rtdbd_vsoa_token_entry_t;

/* per-cli 会话上限（与 §8.3-4 的 token 表上限 32 对齐）。 */
#define RTDBD_VSOA_MAX_CLI 32u
/* token 表上限（§8.3-4：32 条上限、零动态分配）。 */
#define RTDBD_VSOA_MAX_TOKEN 32u

/* 注入写路径依赖（§8.3-4：plc 的写请求过 irt_policy_allows +
 * rtdbd_conf_partition_allowed，与 UDS **同一函数**、同一 deny-by-default）。
 * 未注入 policy/conf 时写路径回 NOT_IMPLEMENTED —— 显式未实现，绝不默认放行。 */
void rtdbd_vsoa_adapter_set_policy(rtdbd_vsoa_adapter_t *a, const irt_policy_t *policy);
void rtdbd_vsoa_adapter_set_conf(rtdbd_vsoa_adapter_t *a, const rtdbd_conf_t *conf);
void rtdbd_vsoa_adapter_set_audit(rtdbd_vsoa_adapter_t *a, rtdbd_vsoa_audit_fn fn, void *ctx);

/* 设置连接默认 uid（**仅限本机 loopback 开发/联调**，且需同时开启
 * set_allow_unauthenticated(true)）。生产形态必须用 hello 握手 + token 表。 */
void rtdbd_vsoa_adapter_set_default_uid(rtdbd_vsoa_adapter_t *a, uint32_t uid);

/* 允许**未握手**连接使用 default_uid 写/订阅（默认关闭）。
 * ⚠ 该开关仅供本机联调；生产必须保持关闭（否则任何能连上的人都能以默认身份写点）。 */
void rtdbd_vsoa_adapter_set_allow_unauthenticated(rtdbd_vsoa_adapter_t *a, bool allow);

/* 登记一条预共享 token（§8.3-4）。返回 0 成功；-1 表满；-2 参数非法。
 * 同一 token 重复登记 → 更新该条目（不新增）。 */
int rtdbd_vsoa_adapter_add_token(rtdbd_vsoa_adapter_t *a, const char *token,
                                  uint32_t uid, uint32_t partition,
                                  const char *service_id, rtdbd_session_role_t role);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_VSOA_ADAPTER_H_ */
