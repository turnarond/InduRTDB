/**
 * @file rtdbd_digest.h
 * @brief S1：确定性点集摘要纯函数（P10 terminal_hash / P11 digest_match 地基）。
 *
 * 输入为点快照数组（不依赖 indurtdb_t 内部、不改 lib API/ABI）；
 * FNV-1a-64 字节累积，结构段与值段分离；按 type 定 value union 有效字节，
 * 避免尾部未初始化污染。算法版本标记 `fnv1a64-v1:` 防止跨版本误判。
 */
#ifndef RTDBD_DIGEST_H_
#define RTDBD_DIGEST_H_

#include <stdint.h>
#include <stdbool.h>

#include <indurtdb/indurtdb.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_DIGEST_HEX_LEN 96u

typedef struct {
    uint64_t struct_hash;   /* (id,partition,name,type,access) 累积 */
    uint64_t value_digest;  /* (id,type,有效 value 字节,quality,source_ts) 累积 */
    uint32_t n_points;
    char hex[RTDBD_DIGEST_HEX_LEN]; /* fnv1a64-v1:<struct:016x>:<value:016x>:<n> */
} rtdbd_digest_t;

/* 计算摘要。points 可为 NULL 仅当 count==0；成功 true。 */
bool rtdbd_digest_compute(const indurtdb_point_t *points, uint32_t count,
                          rtdbd_digest_t *out);
/* 逐字段精确比较（非字符串前缀）。 */
bool rtdbd_digest_equal(const rtdbd_digest_t *a, const rtdbd_digest_t *b);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_DIGEST_H_ */
