/**
 * @file rtdbd_device_agent.h
 * @brief h-C 设备侧代理（新组件）：对外部 PC 暴露 VSOA server，自身以 2 条
 *        loopback VSOA client 连 rtdbd（hello 合规），把"外部来源"转为"设备本地来源"。
 *
 * 设计（28 号方案 + v3.12-hC主体-设备侧代理实现卡 v2）：
 * - 双角色同进程、单线程 poll（spike 实证可行）；
 * - 对外 passwd 连接级准入（d-C）、绑指定管理网卡 IP（d-A，拒绝 0.0.0.0）；
 * - N:2 聚合：client#1 plc 读 4 只读 URL + 13 镜像只读子集 + 订阅管理，
 *   client#2 sync_admin 仅 /sync/peers/get（d-B）；
 * - 并发 pending 表（cli_id+seq+tun 索引、16 项、3s 超时）+ 异步两阶段 reply；
 * - RBE 语义适配：per-cli 订阅集 + per-point 引用计数 + 首值缓存 + 二次扇出；
 * - 诊断计数出口（对齐 vsoa_adapter.h notify_counters）。
 *
 * 不复用 vsoa_adapter（职责不同：代理是中转、adapter 是业务前端）。
 * 不依赖 vsoa 头（本头仅用不透明前向声明），SDK 类型只在 .c 内可见。
 */
#ifndef RTDBD_DEVICE_AGENT_H_
#define RTDBD_DEVICE_AGENT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明类型：SDK vsoa_server_t / vsoa_client_t 只在 .c 内可见。 */
typedef struct rtdbd_device_agent rtdbd_device_agent_t;

/* 对外 server 与上游 rtdbd 的连接参数（均由命令行/环境注入，不硬编码）。 */
typedef struct {
    const char *bind_ip;        /* d-A：指定管理网卡 IP（拒绝 0.0.0.0/NULL） */
    uint16_t    port;           /* 0 = 自动分配，经 port-file 回读 */
    const char *port_file;      /* 独立 RTDBD_AGENT_PORT_FILE */
    const char *passwd;         /* d-C：连接级共享口令（NULL → 创建失败、fail-closed） */
    const char *upstream_host;  /* 上游 rtdbd 地址（通常 127.0.0.1） */
    uint16_t    upstream_port;  /* 上游 rtdbd VSOA 端口 */
    const char *token_ro;       /* client#1 plc token（rtdbd 侧预置） */
    const char *token_admin;    /* client#2 sync_admin token（rtdbd 侧预置） */
    uint32_t    max_listeners;  /* u-E：并发对外 cli 上限（≤16） */
} rtdbd_agent_cfg_t;

/* 诊断计数出口（对齐 vsoa_adapter.h:75 notify_counters 模式）。 */
typedef struct {
    uint64_t replied;          /* 成功回写外部 client 的 RPC 数 */
    uint64_t dropped;          /* NOTIFY 慢消费者丢弃数 */
    uint64_t evicted;          /* 首值缓存淘汰数 */
    uint64_t pending_full;     /* 在途表满被拒数 */
    uint64_t upstream_reconnect;/* 上游重连次数 */
    uint64_t denied;           /* 鉴权/白名单拒绝数 */
} rtdbd_agent_stats_t;

/* 创建代理：校验 cfg（bind_ip 非 0.0.0.0、passwd 非空、max_listeners<=16），
 * 起对外 server（含 passwd）、连 2 条 loopback client 并 hello；失败返回 NULL。 */
rtdbd_device_agent_t *rtdbd_device_agent_create(const rtdbd_agent_cfg_t *cfg);
void rtdbd_device_agent_destroy(rtdbd_device_agent_t *a);

/* 驱动一轮事件：合并 server + 2 client fd_set，poll + input_fds；返回是否有事件。
 * 由 main 循环调用直到 g_stop；回调在其内部触发。 */
bool rtdbd_device_agent_poll_once(rtdbd_device_agent_t *a, int timeout_ms);

/* 诊断计数快照（线程内单线程事件循环调用）。 */
rtdbd_agent_stats_t rtdbd_device_agent_get_stats(const rtdbd_device_agent_t *a);

/* ---- RBE 适配纯逻辑（OFF 单测可直测；无 SDK 依赖）---- */
/* per-point 引用计数 + 首值缓存 + 二次过滤的最小状态机。
 * 纯逻辑函数不依赖 SDK/传输，供 test 直接断言 A/B 三例。 */
#define RTDBD_AGENT_FIRST_CACHE_MAX 10000u

typedef struct {
    uint32_t point_id;
    uint32_t refcnt;          /* 订阅该点的 listener 数 */
    bool     has_cached;
    uint8_t  cached[32];      /* rtdbd_notify_t POD 原样（32B，见 protocol.h:167-175） */
} agent_point_t;

/* per-listener 订阅位图：RTDBD_SUB_MAX 点 → 1250B，16 listener 共 20KB（有界）。 */
#define RTDBD_AGENT_LISTENER_MAX 16u
#define RTDBD_AGENT_SUB_BMP_WORDS ((RTDBD_AGENT_FIRST_CACHE_MAX + 31u) / 32u)

typedef struct {
    /* per-listener 订阅集（位图，point_id 0..RTDBD_AGENT_FIRST_CACHE_MAX-1） */
    uint32_t listener_bmp[RTDBD_AGENT_LISTENER_MAX][RTDBD_AGENT_SUB_BMP_WORDS];
    agent_point_t *points;    /* 动态分配 n_points 项（首值缓存，按实际点数受控） */
    uint32_t n_points;
    uint32_t cap_points;
    uint64_t evicted;
} agent_rbe_state_t;

void agent_rbe_init(agent_rbe_state_t *s);
/* 返回 true 表示这是首个 listener 订阅该点（需向上游 /subscription/add）。 */
bool agent_rbe_subscribe(agent_rbe_state_t *s, uint32_t listener, uint32_t point_id);
/* 返回 true 表示这是最后 listener 退订该点（需向上游 /subscription/remove）。 */
bool agent_rbe_unsubscribe(agent_rbe_state_t *s, uint32_t listener, uint32_t point_id);
/* 该 listener 是否订阅了 point_id（二次过滤判据）。 */
bool agent_rbe_listener_has(const agent_rbe_state_t *s, uint32_t listener, uint32_t point_id);
/* 存首值缓存（满则淘汰最旧、计 evicted）。 */
void agent_rbe_cache(agent_rbe_state_t *s, uint32_t point_id, const uint8_t notify[32]);
/* 取缓存首值（供新 listener 订阅时主动推）。 */
bool agent_rbe_get_cached(const agent_rbe_state_t *s, uint32_t point_id, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_DEVICE_AGENT_H_ */
