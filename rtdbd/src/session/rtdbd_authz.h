/**
 * @file rtdbd_authz.h
 * @brief VSOA 前端身份/来源授权判据（doc 13 §8.3-4，M3.x / P7 鉴权）。
 *
 * 为什么抽出：§8.3-4 的「plc / sync_admin 仅 loopback」「sync_peer 仅 /sync/
 * 前缀」「sync_admin 只能做成员管控」原本内联在 vsoa_adapter.c 的命令回调里，
 * 与 VSOA SDK 的 sockaddr/URL 结构耦合，无法在不建真实跨机链路的前提下证伪。
 * 本模块把判据降为**标量 + C 字符串**的纯函数，使现场才有的地址/角色组合
 * 可在本机用构造值真实验证；adapter 负责把 SDK 结构归一化后调用本模块。
 *
 * 传输无关约束：零 fd/socket/poll/vsoa_* 符号，不 include 任何 VSOA SDK 头；
 * 地址以**主机序** uint32 传入，避免引入 arpa/inet.h 等平台头。
 */
#ifndef RTDBD_AUTHZ_H_
#define RTDBD_AUTHZ_H_

#include <stdbool.h>
#include <stdint.h>

#include "session/rtdbd_session.h" /* rtdbd_session_role_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 来源强校验（§8.3-4）。
 * @param role       token 表登记的角色
 * @param is_ipv4    对端地址是否为 AF_INET
 * @param addr_host  IPv4 地址（**主机序**，如 0x7F000001u）
 * @return RTDBD_ST_OK / RTDBD_ST_DENIED / RTDBD_ST_NOT_IMPLEMENTED
 *         sync_peer 的来源校验依赖运行时成员表，当前显式 NOT_IMPLEMENTED
 *         （宁可显式未实现，也不默认放行）。
 */
uint16_t rtdbd_authz_source_allowed(rtdbd_session_role_t role,
                                     bool is_ipv4, uint32_t addr_host);

/**
 * 已握手会话的 role → bare URL 授权（§8.3-4 deny-by-default）。
 * bare URL 为去掉版本段后的路径，如 "/point/write"、"/sync/peers/set"。
 *
 * 矩阵：
 *   /hello                     任意角色允许（握手入口）
 *   PLC                        禁止同步前缀 URL，其余镜像能力允许
 *   SYNC_ADMIN                 仅 /sync/peers/set 与 /sync/peers/get，不得写点位
 *   SYNC_PEER                  仅同步前缀 URL（禁止 /point/write 等，防回环）
 *   UNKNOWN / 空 URL           拒绝
 *
 * 仅对**已握手**会话调用；未握手路径沿用 adapter 既有的 cred_ok 判据，
 * 本轮不扩大变更面（避免影响 allow_unauthenticated 的本机联调用例）。
 */
bool rtdbd_authz_url_allowed(rtdbd_session_role_t role, const char *bare_url);

#ifdef __cplusplus
}
#endif

#endif /* RTDBD_AUTHZ_H_ */
