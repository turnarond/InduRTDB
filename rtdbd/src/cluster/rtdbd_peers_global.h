/**
 * @file rtdbd_peers_global.h
 * @brief v3.12 M3.3：成员表**进程级单例**访问器（UDS / VSOA 两前端共用）。
 *
 * 动机：M3.2 把成员表放在 rtdbd.c 的静态全局，VSOA adapter 是另一编译单元，
 * 若各自持有一张表会违反 §8.3-6「进程内单例」。这里下沉到 cluster 模块，
 * 两前端通过同一访问器读写，零锁（主循环单线程）。
 */
#ifndef RTDBD_PEERS_GLOBAL_H_
#define RTDBD_PEERS_GLOBAL_H_

#include <stdbool.h>

#include "rtdbd_peer_table.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 进程级成员表（静态存储期零初始化等价 init）。 */
rtdbd_peer_table_t *rtdbd_peers_global(void);

/* 本节点地址；空串表示未配置 → 不做「拒绝自地址」校验。 */
const char *rtdbd_peers_self_addr(void);

/* 设置本节点地址：非法 / 超长返回 false 且**不改写**旧值。 */
bool rtdbd_peers_set_self_addr(const char *addr);

/* 本节点启动标识（IR6 proc_restart_no_fullsync：boot_id 变化即「非首次」启动）。
 * 由 rtdbd.c 启动时 rtdbd_boot_id_generate() 生成并 set；纯取值/赋值，零锁。 */
uint64_t rtdbd_peers_boot_id(void);
void     rtdbd_peers_set_boot_id(uint64_t id);
/* 生成启动标识：CLOCK_MONOTONIC 纳秒高位 + 随机低位，每次启动高概率不同。 */
uint64_t rtdbd_boot_id_generate(void);

/* 本节点已应用偏移参考（最小 lag 计算用；同步流应用 delta 时推进，默认 0）。
 * ⚠ lag 精确语义（degraded 等）§8.3-6 协议未裁决，本切片仅落地最小可观测差值
 *（lag = local_offset − peer.applied_offset，饱和 0），不假装完整 IR6 语义。 */
uint64_t rtdbd_peers_local_offset(void);
void     rtdbd_peers_set_local_offset(uint64_t off);

/* 整组替换 + ADD/DEL diff 入延迟动作队列（UDS 与 VSOA **共用同一实现**）。
 * 返回 0 成功；-1 表示参数非法（自地址/非法地址/超限/重复）且**旧表不变**。
 * 成功后新 peer 会额外入队 TRIGGER_FULLSYNC（§8.3-6 on_join 全量）。 */
int rtdbd_peers_apply_set(const rtdbd_peer_t *peers, uint32_t n);

/* 从 --peers-file 加载**初始成员表**（配置来源，非运行时状态）。返回 0 成功；
 * -1 表示文件无法打开 / 解析失败 / 含自地址 / 超限。走与运行态相同的
 * rtdbd_peers_apply_set（整组替换 + 自地址拒绝 + diff 入延迟队列）。 */
int rtdbd_peers_load_file(const char *path);



#ifdef __cplusplus
}
#endif
#endif /* RTDBD_PEERS_GLOBAL_H_ */
