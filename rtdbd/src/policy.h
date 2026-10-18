/**
 * @file policy.h
 * @brief 写权限策略表：uid × 分区 → 允许写入的点位区间
 *
 * 配置格式（每行一条；`#` 起始到行尾为注释，空行忽略）：
 *   <uid>:<partition|*>:<min_point_id>:<max_point_id>
 * 四段一律十进制；第 2 段写 `*` 表示「作用于所有分区」。区间为闭区间。
 * 例：
 *   0:*:0:4294967295        # root：所有分区全段
 *   1000:1:0:999            # uid 1000 仅分区 1 的 id 0..999
 *   1001:*:1000:1999        # uid 1001 所有分区的 id 1000..1999
 *
 * 匹配语义：先匹配 uid，再匹配分区（本行 partition 为 `*` 时跳过该步），
 * 最后判 id 闭区间。默认拒绝：不在任何规则内的请求一律拒绝
 * （deny by default）。
 *
 * 校验为 fail-fast：任何一行非法（段数 != 4、空段、非十进制数字、数值
 * 溢出 uint32、`min_id > max_id`、单行超 IRT_POLICY_LINE_MAX、规则数超
 * IRT_POLICY_MAX_RULES）都会使加载以负值失败，调用方（rtdbd）须以配置
 * 校验退出码终止，**不得静默跳过**——静默缺失会让「写不进去」变成无从
 * 察觉的哑故障。
 *
 * 运维约束：策略**仅在 rtdbd 启动时加载一次**（irt_policy_load）。
 * 运行时修改策略文件不会即时生效，须**重启 rtdbd 进程**方能应用新规则。
 * 当前版本不提供策略热加载 / SIGHUP 重载。
 */
#ifndef RTDBD_POLICY_H_
#define RTDBD_POLICY_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRT_POLICY_MAX_RULES 32
#define IRT_POLICY_LINE_MAX  128

typedef struct {
    uint32_t uid;
    uint32_t partition;          /* wildcard_partition 为 1 时无意义 */
    uint32_t min_id;
    uint32_t max_id;
    int      wildcard_partition; /* 1 = 本行作用于所有分区 */
} irt_policy_rule_t;

typedef struct {
    irt_policy_rule_t rules[IRT_POLICY_MAX_RULES];
    size_t            count;
} irt_policy_t;

/* irt_policy_load 的结果码 */
enum {
    IRT_POLICY_OK          = 0,
    IRT_POLICY_ERR_ARG     = -1, /* 空指针参数 */
    IRT_POLICY_ERR_OPEN    = -2, /* 策略文件打不开 */
    IRT_POLICY_ERR_TOOMANY = -3, /* 规则数超过 IRT_POLICY_MAX_RULES */
    IRT_POLICY_ERR_FIELDS  = -4, /* 段数不是 4 */
    IRT_POLICY_ERR_NUMBER  = -5, /* 空段 / 非十进制数字 / 溢出 uint32 */
    IRT_POLICY_ERR_RANGE   = -6, /* min_id > max_id */
    IRT_POLICY_ERR_LINELEN = -7, /* 单行超过 IRT_POLICY_LINE_MAX */
};

void irt_policy_init(irt_policy_t* p);

/* 加载策略文件。成功返回 IRT_POLICY_OK(0)，失败返回上面的负值。
 * err_line 非空时写入出错的行号（从 1 起；非行定位类错误如打不开文件
 * 写 0）。 */
int irt_policy_load(irt_policy_t* p, const char* path, uint32_t* err_line);

/* 结果码的可读原因文本（供日志打印，仿 indurtdb_cfg_error_reason）。 */
const char* irt_policy_strerror(int rc);

/* 是否允许 uid 在 partition 上对 point_id 执行管理写。
 * 返回 1 允许，0 拒绝（deny by default）。 */
int irt_policy_allows(const irt_policy_t* p, uint32_t uid,
                      uint32_t partition, uint32_t point_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* RTDBD_POLICY_H_ */
