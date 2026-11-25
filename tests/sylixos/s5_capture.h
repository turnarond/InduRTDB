/**
 * @file s5_capture.h
 * @brief S5 取证 JSON 构建/来源标注/校验纯函数（host 单测与板载程序共用同一 TU）。
 *
 * 字段来源硬标注：on-site（现场真取）/ logic-only（禁填 measured）/
 * unavailable（null + reason）。P10 三字段与 P11 machine_restart_pull_full
 * 在产品语义不足时必须 unavailable，禁止 on-site。
 */
#ifndef S5_CAPTURE_H_
#define S5_CAPTURE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S5_CAPTURE_MAX_FIELDS 64u
#define S5_CAPTURE_VALUE_LEN  8192u  /* S5b：承载 4104B peers 摘要 + 余量 */
#define S5_CAPTURE_REASON_LEN 160u

typedef enum {
    S5_SRC_NONE = 0,
    S5_SRC_ON_SITE,
    S5_SRC_LOGIC_ONLY,
    S5_SRC_UNAVAILABLE
} s5_source_t;

typedef struct {
    char        key[48];
    char        value[S5_CAPTURE_VALUE_LEN]; /* JSON 值片段（数字或带引号字符串或 null） */
    s5_source_t source;
    char        reason[S5_CAPTURE_REASON_LEN];
} s5_field_t;

typedef struct {
    char       mode[16];    /* f2 / f3a / f4 */
    char       node_id[64];
    s5_field_t fields[S5_CAPTURE_MAX_FIELDS];
    uint32_t   n_fields;
} s5_capture_t;

void s5_capture_init(s5_capture_t *c);
bool s5_field_set_u64(s5_capture_t *c, const char *key, uint64_t v, s5_source_t src);
bool s5_field_set_on_site(s5_capture_t *c, const char *key, const char *json_value);
bool s5_field_set_logic_only(s5_capture_t *c, const char *key,
                             const char *json_value, const char *reason);
bool s5_field_set_unavailable(s5_capture_t *c, const char *key, const char *reason);
s5_source_t s5_field_source(const s5_capture_t *c, const char *key);

/* 生成 JSON：{"mode":"..","node_id":"..","fields":{"k":{"value":..,"_source":"..","reason":".."}}} */
bool s5_capture_emit_json(const s5_capture_t *c, char *buf, size_t cap);
/* 校验：unavailable 必须有 reason；P10 三字段/P11 machine_restart_pull_full 不得 on-site；
 * logic-only 不得冒用 P10 字段名；字段数/长度有界。 */
bool s5_capture_validate(const s5_capture_t *c);
/* S5b：RPC status 落值（on-site），如 deny_bad_token=0x83。 */
bool s5_field_set_status(s5_capture_t *c, const char *key, uint8_t status, s5_source_t src);
/* S5b：主机侧 F4 两态比对——输入两份 emit JSON，输出 {"digest_match":bool,"peers_restored":bool}。 */
bool s5_f4_compare(const char *before_json, const char *after_json,
                   char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* S5_CAPTURE_H_ */
