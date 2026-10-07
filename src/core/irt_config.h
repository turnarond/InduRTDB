/**
 * @file irt_config.h
 * @brief 配置加载器 (key=value 初始化参数 + YAML 点位元数据)
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_CONFIG_H_
#define IRT_CORE_IRT_CONFIG_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <indurtdb/indurtdb.h>

#define IRT_CONFIG_ID_MAX   64
#define IRT_CONFIG_LINE_MAX 256

typedef struct {
    char     instance_id[IRT_CONFIG_ID_MAX];
    uint32_t max_points;
    uint32_t max_subscribers;
} irt_config_t;

void irt_config_init_defaults(irt_config_t* cfg);
int  irt_config_load_file(irt_config_t* cfg, const char* path);

/* ---- YAML 点位元数据解析 (SRS §3.3) ---- */

typedef struct {
    uint32_t id;
    uint8_t  type;
    uint16_t unit;
    uint8_t  access;
    char     name[64];
    /* v3.7 主题B B3：可选点位语义（YAML 中给出 eur_min/eur_max/deadband/flags
     * 时写入共享元数据区，供 indurtdb_validate_config 启动即校验）。
     * has_meta=0 表示配置未声明语义，保持段内原值不动（向后兼容）。 */
    double   eur_min;
    double   eur_max;
    float    deadband;
    uint32_t flags;
    uint8_t  has_meta;
    /* 某一可选字段的值无法解析（非数字 / 溢出 / NaN）→ 置 1。
     * 调用方须据此把配置判为非法（fail-fast），
     * 否则 `deadband: abc` 会被静默当成 0 而"看起来合法"。 */
    uint8_t  bad_value;
} irt_point_meta_t;

typedef struct {
    irt_point_meta_t* points;
    size_t            count;
    size_t            capacity;
} irt_point_meta_batch_t;

/** 解析 YAML 点位配置文件, 返回载入的点数 (负值=错误) */
int  irt_point_config_parse_yaml(const char* path, irt_point_meta_batch_t* out);
void irt_point_config_free(irt_point_meta_batch_t* batch);
uint8_t irt_config_parse_type(const char* s);

#endif /* IRT_CORE_IRT_CONFIG_H_ */
