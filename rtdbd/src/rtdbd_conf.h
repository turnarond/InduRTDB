/**
 * @file rtdbd_conf.h
 * @brief rtdbd 私有配置：点位分区白名单与别名（v3.9 T5）
 *
 * 与 `--config` 传入的点位 YAML 是**同一个文件**：本解析器只读 `points:`
 * 段**之前**的可选顶层键，不触碰点位段（点位段由核心 `indurtdb_h_load_config`
 * 解析）。刻意放在 rtdbd 侧，**不向公共 API 增加任何函数**（T5「MUST NOT 改
 * 公共 API」约束）。
 *
 * 支持的键（内联形式，见设计方案 §3.5）：
 *   partitions: [0, 1, 2]                    十进制 uint32 白名单
 *   partition_alias: {0: "default", 1: "unitA"}   uint32 → 字符串别名
 *
 * 语义（fail-closed）：
 *   - `partitions` 缺省或为空列表 ⇒ 任意分区值合法（v3.8 等价宽松）；
 *   - 非空白名单 ⇒ 不在表内的分区值非法。
 *   - 存在白名单时，每个 `partition_alias` 键都必须落在白名单内，否则配置错误。
 *   - `partition_alias` 仅供后续（T7）展示用，T5 无协议面。
 *
 * 任何格式错误都应使 rtdbd 以配置校验退出码（RTDBD_EXIT_CONFIG）终止 ——
 * 与 policy / 点位段 fail-fast 同一约定，绝不静默忽略。
 */
#ifndef RTDBD_CONF_H_
#define RTDBD_CONF_H_

#include <stdint.h>

#include <rtdbd/protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_CONF_MAX_PARTITIONS 64u
#define RTDBD_CONF_MAX_ALIASES    64u
#define RTDBD_CONF_ALIAS_MAX      64u

/* v3.11 容量默认值/硬上限；max_batch 与 max_subs 复用协议硬上限。 */
#define RTDBD_CONF_MAX_CLIENTS          128u
#define RTDBD_CONF_DEFAULT_MAX_CLIENTS  128u
#define RTDBD_CONF_DEFAULT_INITIAL_CLIENTS 32u
#define RTDBD_CONF_DEFAULT_MAX_SUBS     RTDBD_SUB_MAX_PER_CONN
#define RTDBD_CONF_DEFAULT_INITIAL_SUBS 1024u
#define RTDBD_CONF_DEFAULT_MAX_BATCH    RTDBD_BATCH_MAX

/* caps_present：区分“未配置”与“显式配置为默认值”。 */
#define RTDBD_CONF_HAS_MAX_CLIENTS          (1u << 0)
#define RTDBD_CONF_HAS_INITIAL_CLIENTS      (1u << 1)
#define RTDBD_CONF_HAS_MAX_SUBS_PER_CONN    (1u << 2)
#define RTDBD_CONF_HAS_INITIAL_SUBS         (1u << 3)
#define RTDBD_CONF_HAS_MAX_BATCH            (1u << 4)

typedef struct {
    uint32_t key;
    char     name[RTDBD_CONF_ALIAS_MAX];
} rtdbd_partition_alias_t;

typedef struct {
    uint32_t partitions[RTDBD_CONF_MAX_PARTITIONS];
    uint32_t n_partitions;   /* 0 = 未配置/空白名单（任意分区值合法） */
    rtdbd_partition_alias_t aliases[RTDBD_CONF_MAX_ALIASES];
    uint32_t alias_line[RTDBD_CONF_MAX_ALIASES]; /* 别名所在行号（校验报错用） */
    uint32_t n_aliases;

    /* v3.11 容量（内置默认值，YAML/CLI 可覆盖）。 */
    uint32_t max_clients;
    uint32_t initial_clients;
    uint32_t max_subs_per_conn;
    uint32_t initial_subs_per_conn;
    uint32_t max_batch;
    uint32_t caps_present;
} rtdbd_conf_t;

void rtdbd_conf_init(rtdbd_conf_t* c);

/* 解析 path 的 `points:` 之前的分区键。
 * 返回：0 成功；-1 格式错误（*err_line 写行号，可为 NULL）；-2 参数/打开失败。
 * path 为 NULL 时视为无配置（成功、空白名单）。 */
int rtdbd_conf_load(rtdbd_conf_t* c, const char* path, uint32_t* err_line);

/* 白名单判定：返回 1 允许，0 拒绝。白名单为空恒返回 1。 */
int rtdbd_conf_partition_allowed(const rtdbd_conf_t* c, uint32_t p);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RTDBD_CONF_H_ */
