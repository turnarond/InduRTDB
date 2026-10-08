/**
 * @file irt_delta.h
 * @brief 运行时点位变更的 delta 日志（v3.7 主题B B4：配置持久化）
 *
 * 设计约束：
 * - 解决「v3.6 CRUD 重启即丢」：运行时 CREATE/DELETE/RENAME 追加写定长记录，
 *   启动时在 load_config(base) 之后回放，使点位跨重启存在。
 * - **base 配置不可变**：不回写 YAML（破坏审计性且并发不安全），只追加 delta。
 * - 定长记录、零堆、无第三方依赖；追加写 + fsync。
 * - 崩溃安全：记录定长 80B；回放时末尾不足 80B 的残片丢弃，magic/version 非预期即停止。
 */
#ifndef IRT_CORE_IRT_DELTA_H_
#define IRT_CORE_IRT_DELTA_H_

#include <stdint.h>

#define IRT_DELTA_MAGIC    0x444C5441u   /* "DLTA" */
#define IRT_DELTA_VERSION  1u

#define IRT_DELTA_OP_CREATE 1u
#define IRT_DELTA_OP_DELETE 2u
#define IRT_DELTA_OP_RENAME 3u

/* delta 记录 80B（定长，POD，无指针）。布局锁死。 */
typedef struct {
    uint32_t magic;      /* 0  : IRT_DELTA_MAGIC */
    uint16_t version;    /* 4  : IRT_DELTA_VERSION */
    uint16_t op;         /* 6  : IRT_DELTA_OP_* */
    uint32_t point_id;   /* 8  */
    uint8_t  type;       /* 12 */
    uint8_t  access;     /* 13 */
    uint8_t  reserved[2];/* 14 */
    char     name[64];   /* 16 : CREATE/RENAME 用新名；DELETE 忽略 */
} irt_delta_rec_t;

/* 回放回调：返回 0 继续；非 0 终止回放并回传该值。
 * ctx 由调用方传入（通常为 indurtdb_t*）。 */
typedef int (*irt_delta_apply_fn)(const irt_delta_rec_t* rec, void* ctx);

/* 打开（或创建）delta 文件用于追加写。返回 fd（>=0），失败返回 -1。 */
int irt_delta_open_append(const char* path);

/* 追加一条记录并 fsync。返回 0 成功；非 0 失败。 */
int irt_delta_append(int fd, const irt_delta_rec_t* rec);

/* 回放 delta 文件：逐条校验并交给 apply。
 * 返回已成功应用的记录数（>=0）；**读错误返回 -1**（与"无历史"的 0 明确区分）。
 * 文件不存在返回 0（视为无历史）。
 * 遇残片（末尾不足一条）或非法 magic/version/op 时停止，已应用的记录保持生效。 */
int irt_delta_replay(const char* path, irt_delta_apply_fn apply, void* ctx);

#endif /* IRT_CORE_IRT_DELTA_H_ */
