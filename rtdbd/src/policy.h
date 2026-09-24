/**
 * @file policy.h
 * @brief 写权限策略表：uid → 允许写入的点位区间
 *
 * 配置格式（每行一条，# 开头为注释）：
 *   <uid>:<min_point_id>:<max_point_id>
 * 例：
 *   1000:0:999
 *   1001:1000:1999
 *
 * 默认拒绝：不在任何规则内的 uid 一律拒绝（deny by default）。
 * 定长数组，无堆分配。
 */
#ifndef RTDBD_POLICY_H_
#define RTDBD_POLICY_H_

#include <stddef.h>
#include <stdint.h>

#define IRT_POLICY_MAX_RULES 32
#define IRT_POLICY_LINE_MAX  128

typedef struct {
    uint32_t uid;
    uint32_t min_id;
    uint32_t max_id;
} irt_policy_rule_t;

typedef struct {
    irt_policy_rule_t rules[IRT_POLICY_MAX_RULES];
    size_t            count;
} irt_policy_t;

void irt_policy_init(irt_policy_t* p);

/* 加载策略文件。成功返回 0；失败返回负值 */
int irt_policy_load(irt_policy_t* p, const char* path);

/* 是否允许。返回 1 允许，0 拒绝 */
int irt_policy_allows(const irt_policy_t* p, uint32_t uid, uint32_t point_id);

#endif /* RTDBD_POLICY_H_ */
