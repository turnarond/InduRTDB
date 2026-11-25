/**
 * @file rtdbd_select_source.h
 * @brief S3 逻辑选源器（纯逻辑，无 VSOA/SDK）。
 *
 * 语义严格限定：13 号:198「(epoch,node_id) 字典序只用于选全量源与审计排序，
 * 不做逐写合并」。本模块只选源 + 出摘要，**不落 shm、不宣称数据收敛**。
 * 输出字段名与 P10（nodes_converged/terminal_hash/dict_max_snapshot_match）隔离。
 */
#ifndef RTDBD_SELECT_SOURCE_H_
#define RTDBD_SELECT_SOURCE_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t epoch;
    char     node_id[32];
    uint64_t boot_id;          /* 仅审计，不参与选源比较（24 号:87 boot_id 非真值） */
    uint64_t struct_hash;
    uint64_t value_digest;
    uint32_t n_points;
    uint64_t offset;           /* 去重键第三元（与 candidate_set 对齐） */
    bool     valid;
} rtdbd_source_candidate_t;

typedef struct {
    uint64_t winner_epoch;
    char     winner_node_id[32];
    char     winner_digest_hex[96];          /* 胜者候选摘要（来自候选注入） */
    char     selected_source_digest_hex[96]; /* 本机摘要（调用方独立注入，非恒等） */
    uint32_t valid_candidate_count;          /* 参与选源的 valid 数，非"收敛节点数" */
    uint32_t candidate_count;
    uint32_t duplicate_dropped;
    uint32_t truncated;
} rtdbd_source_result_t;

/* 全量候选中按归一化 (epoch,node_id) 取最大；boot_id 不参与。
 * local_digest_hex 为本机摘要（独立数据源），NULL 时回退 winner_digest_hex。
 * 成功 true；cands 为空或 out 为 NULL 返回 false。 */
bool rtdbd_select_source(const rtdbd_source_candidate_t *cands, uint32_t n,
                         rtdbd_source_result_t *out, const char *local_digest_hex);

/* JSON 结论（evidence_kind:"logic-only"；禁含 P10 三字段名）。 */
bool rtdbd_select_source_json(const rtdbd_source_result_t *res, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_SELECT_SOURCE_H_ */
