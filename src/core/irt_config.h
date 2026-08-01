/**
 * @file irt_config.h
 * @brief 配置加载器 (key=value 初始化参数 + YAML 点位元数据)
 * @version 3.0.0
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
