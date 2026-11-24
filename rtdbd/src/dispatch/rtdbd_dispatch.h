/**
 * @file rtdbd_dispatch.h
 * @brief rtdbd transport 无关核心分发表（M1：双前端等价协议层）
 *
 * 设计约束（doc 13 §7/§7.4）：
 * - 本模块**禁止**出现 socket、fd、poll、vsoa_*、rtdbd_conn_t、outq_push、SO_PEERCRED
 *   等 transport 或 VSOA 符号（由 scripts/check_dispatch_boundary.py 机械校验）。
 * - 同一 (opcode, req) 必须在该函数内产生**确定性**的 (status, resp 体)；
 *   transport 适配器（UDS / VSOA）只负责填充 12B 响应头（magic/version/
 *   status=rtdbd_st_to_vsoa_status(st)/payload_len）并发送。两前端喂相同输入，
 *   必须产出字节一致的完整响应——这是 §7.4 的 dispatch 等价性。
 * - 第一步仅落地无状态的 PING 与纯映射（URL↔opcode、status 映射）；
 *   涉及点表/订阅/鉴权的 opcode 暂返回 NOT_IMPLEMENTED，后续里程碑把 rtdbd.c
 *   的 op_* 逻辑逐步迁移到此并接入 ctx，UDS 侧 22 opcode 与 ABI 零变化。
 */
#ifndef RTDBD_DISPATCH_H_
#define RTDBD_DISPATCH_H_

#if defined(__cplusplus)
#include <cstdint>
#include <cstddef>
#include <rtdbd/protocol.h>
#include <indurtdb/indurtdb.h>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <rtdbd/protocol.h>
#include <indurtdb/indurtdb.h>
#endif

/* 策略与配置（纯业务模块，非传输）：写路径鉴权需与 UDS 侧**同一函数**、同一
 * deny-by-default（doc 13 §8.3-4）。 */
#include "policy.h"
#include "rtdbd_conf.h"
#include "subtab.h" /* 订阅成员 + RBE 状态合并表（通知门控复用） */
#include "logbuf.h" /* h-C 前置：irt_logbuf_t 类型（只读快照纯函数入参） */

#ifdef __cplusplus
extern "C" {
#endif

/* 状态码 → VSOA 帧 status（doc 13 §7.2：7 个 RTDBD_ST_* 无损映射，高位 0x80
 * 区分 rtdbd 语义）。纯函数，transport 无关。 */
uint16_t rtdbd_st_to_vsoa_status(uint8_t st);

/* VSOA URL → rtdbd opcode（doc 13 §7.1 镜像的 13 个能力）。0=识别，-1=未识别。 */
int rtdbd_opcode_from_url(const char *url, uint16_t *opcode);

/* rtdbd opcode → VSOA URL（仅镜像能力；非镜像返回 NULL）。 */
const char *rtdbd_url_from_opcode(uint16_t opcode);

/* URL 归一化：剥离 VSOA 的**版本段前缀**（doc 13 §95「URL 带版本段」），
 * 把 "/indurtdb/v1/ping" 归一为裸能力路径 "/ping"，供 URL↔opcode 表匹配。
 * 无版本段时原样复制；输出缓冲不足或入参非法返回 false。
 * 纯逻辑（不依赖 VSOA），故默认构建（INDURTDB_ENABLE_VSOA=OFF）亦可单测。 */
bool rtdbd_normalize_url(const char *url, char *out, size_t out_cap);

/* ---- 业务语义（纯计算，无传输依赖；UDS / VSOA 两前端共享同一份实现）----
 *
 * 迁移范式（doc 13 §8.3-3「UDS / VSOA 各做一层编解码适配，共享同一套业务
 * 语义与写权威路径」）：业务语义函数只依赖**协议 POD** 与**配置快照**，
 * 不触碰 fd / socket / 连接队列 / 响应头 —— 两端各自负责编解码与发送，
 * 从而「同一 req 在两前端产出字节一致的 resp」可被机械验证。
 */

/* /caps 所需的配置快照（由上层从 rtdbd 运行配置填充）。 */
typedef struct {
    uint32_t max_clients;
    uint32_t initial_clients;
    uint32_t max_subs_per_conn;
    uint32_t initial_subs_per_conn;
    uint32_t max_batch;
} rtdbd_dispatch_cfg_t;

/* 构造 /caps 响应体（rtdbd_caps_t，32B POD，零序列化）。
 * 返回 RTDBD_ST_OK；缓冲不足返回 RTDBD_ST_NO_MEM 且不写越界。 */
uint16_t rtdbd_dispatch_caps(const rtdbd_dispatch_cfg_t *cfg,
                             uint8_t *resp, size_t resp_cap, size_t *resp_len);

/* 读点位当前值并组装 /point/get 响应体（rtdbd_get_resp_t，64B POD）。
 * 语义与既有 UDS 侧 do_get 完全一致：未注册点位（name[0]=='\0'）与越界 id
 * 一律按「未找到」处理（RTDBD_ST_NOT_FOUND）。
 * 返回 RTDBD_ST_OK；缓冲不足返回 RTDBD_ST_NO_MEM 且不写越界。 */
uint16_t rtdbd_dispatch_get(indurtdb_t *h, uint32_t point_id,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len);

/* ---- 写路径（/point/write）的共享语义 ----
 *
 * 拆分边界（doc 13 §8.3-1「VSOA loopback 为现场主写入口」+ §8.3-4「复用
 * irt_policy_allows、同一 deny-by-default」+ §8.3-5 推送定向化）：
 *
 *   环节            | 归属   | 理由
 *   ----------------|--------|---------------------------------------------
 *   请求体解析      | 前端   | 读字节属传输层
 *   凭据获取        | 前端   | UDS=SO_PEERCRED；VSOA=握手绑定（§8.3-4）
 *   鉴权 + 落值     | 本函数 | 两前端**同一函数**、同一 deny-by-default
 *   统计计数        | 本函数 | 同一进程同一份 g_stats
 *   审计记录        | 前端   | 审计对象属前端依赖（irt_audit_record）
 *   变更通知        | 前端   | 投递机制传输特有（UDS 广播 / VSOA 定向）
 *   响应编码 + 发送 | 前端   | 传输层
 *
 * partition 语义与既有 UDS 写路径**一致**：用**连接级默认分区**（UDS 由 OP_SELECT
 * 设置，VSOA 由握手携带），而非点位所属分区。
 */
uint16_t rtdbd_dispatch_write(indurtdb_t *h, const irt_policy_t *policy,
                               const rtdbd_conf_t *conf,
                               const rtdbd_write_req_t *w,
                               uint32_t uid, uint32_t partition,
                               indurtdb_meta_t *out_meta);

/* 订阅鉴权（订阅路径的共享语义）：点位存在性 + 策略 + 分区。
 * ⚠ 分区语义**与写路径不同**：订阅用**点位所属分区**（pt.partition），
 * 写路径用连接级默认分区 —— 两者不可混用。
 * 凭据（pid/uid）来源属前端（UDS=SO_PEERCRED，VSOA=握手绑定），本函数只收 uid。
 * 返回 RTDBD_ST_OK / NOT_FOUND（越界或未注册）/ DENIED（策略或分区不允许）。 */
uint16_t rtdbd_dispatch_subscribe_authz(indurtdb_t *h, const irt_policy_t *policy,
                                         const rtdbd_conf_t *conf,
                                         uint32_t uid, uint32_t point_id);

/* ---- 变更通知的共享语义（NOTIFY 帧组装 + RBE 门控）----
 *
 * §8.3-5：NOTIFY 走**逐连接定向**（cli_datagram），禁用 publish；quick 通道不得
 * 承载增量/快照/ack/握手。投递机制传输特有（UDS 出站队列 / VSOA 定向帧），
 * 但**帧内容与 RBE 门控**是共享语义 —— 两前端必须产出字节一致的通知帧，
 * 且 RBE 抑制状态机必须一致（否则同一死区在两端行为不同）。
 */

/* 组装变更通知帧（rtdbd_notify_t，32B POD，零序列化）。
 * timestamp_ns 与 quality 取**当前点值**（读点）：写路径可能把 TIMEOUT 恢复为
 * GOOD，通知必须携带当前质量而非假定 GOOD。 */
uint16_t rtdbd_dispatch_notify(indurtdb_t *h, const rtdbd_write_req_t *w,
                                rtdbd_notify_t *out_notify);

/* RBE 门控：死区未越过则抑制本次通知（复用既有 subtab 状态机，与 UDS 侧同一
 * 函数、同一状态）。返回 true = 应发送。 */
bool rtdbd_dispatch_notify_rbe(rtdbd_subtab_t *sub, const rtdbd_write_req_t *w,
                               const indurtdb_meta_t *meta);

/* transport 无关核心：给定 opcode + 请求体，产出响应体 + status。
 * 不碰 socket/fd/全局状态；resp 缓冲区由调用方提供（resp_cap 防溢出）。
 * 第一步仅覆盖 PING（无状态）；其余返回 RTDBD_ST_NOT_IMPLEMENTED 占位。 */
uint16_t rtdbd_dispatch(uint16_t opcode,
                        const uint8_t *req, size_t req_len,
                        uint8_t *resp, size_t resp_cap, size_t *resp_len);

/* h-B health 镜像：transport 无关**真纯函数**填 72B rtdbd_health_t。
 * 所有时变/传输输入均由调用方注入：health（RTDBD_HEALTH_*）、8 个计数值、
 * n_conns、conn_table_ok。函数体内不取时钟、不读写全局、不调 self_check。
 * 相同输入 → 两前端 72B 逐字节一致（13 号 §7.4、变更卡判据 3）。
 * rtdbd_health_counts_t 定义在内部头（不进 protocol.h，不碰冻结 ABI）。 */
typedef struct {
    uint64_t n_writes;
    uint64_t n_notifies;
    uint64_t decode_fail;
    uint64_t write_rejected;
    uint64_t write_error;
    uint64_t notify_drop;
    uint64_t notify_send_fail;
    uint64_t uptime_ns;
} rtdbd_health_counts_t;

void rtdbd_dispatch_health_fill(int health, const rtdbd_health_counts_t *counts,
                                uint32_t n_conns, bool conn_table_ok,
                                rtdbd_health_t *out);

/* h-C 前置：4 个只读 URL 的 transport 无关纯函数（UDS/VSOA 共用，输入全注入）。
 * 相同输入 → 两前端产出逐字节一致（13 号 §7.4）。函数体内无 fd/socket/全局状态。
 * 缓冲截断在纯函数内按 cap 完成（handler 只传 cap=sizeof(resp)），两前端一致判据
 * 仅在 max≤阈值时承诺（list 8192/76=107、log 8192/128=64；超阈值 VSOA 按 cap 截断、
 * UDS 保持原上限，例外由交付说明标注）。 */
/* 按名查点：name 为 64B 定长（入口补 '\0'）；分区白名单 fail-closed。
 * 返回 RTDBD_ST_*；成功时 out_id 填点位 id。 */
uint16_t rtdbd_dispatch_find(indurtdb_t *h, const rtdbd_conf_t *conf,
                             uint32_t partition, const char *name /*64B*/,
                             uint32_t *out_id);
/* 读元数据：成功时 out 填 32B indurtdb_meta_t 布局（rtdbd_meta_payload_t）。 */
uint16_t rtdbd_dispatch_meta(indurtdb_t *h, uint32_t point_id,
                             rtdbd_meta_payload_t *out);
/* 枚举点位：分区白名单迁移至此（非法具体分区→OK+空 body、ALL 哨兵不过滤）；
 * do_list 扫描逻辑迁入、max_points 由参数注入（不偷用全局 g_max_points）；
 * 按 cap 截断 max 到 min(max, cap/76)，out_len 返回实际字节数。 */
uint16_t rtdbd_dispatch_list_snapshot(indurtdb_t *h, const rtdbd_conf_t *conf,
                                      uint32_t partition, uint32_t max, uint32_t offset,
                                      uint32_t max_points, uint8_t *buf, size_t cap,
                                      size_t *out_len);
/* 日志环形快照：注入 logbuf 指针；max==0→全部、>RTDBD_LOG_CAPACITY→128，
 * 再按 cap 截断到 min(n, cap/128)；单页快照无游标。 */
uint16_t rtdbd_dispatch_log_snapshot(const irt_logbuf_t *logbuf, uint32_t max,
                                     uint8_t *buf, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_DISPATCH_H_ */
