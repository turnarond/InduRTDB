#include "policy.h"

#include <stdio.h>
#include <string.h>

void irt_policy_init(irt_policy_t* p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

const char* irt_policy_strerror(int rc)
{
    switch (rc) {
    case IRT_POLICY_OK:          return "ok";
    case IRT_POLICY_ERR_ARG:     return "null argument";
    case IRT_POLICY_ERR_OPEN:    return "cannot open policy file";
    case IRT_POLICY_ERR_TOOMANY: return "too many rules";
    case IRT_POLICY_ERR_FIELDS:  return "expected 4 fields: uid:partition|*:min_id:max_id";
    case IRT_POLICY_ERR_NUMBER:  return "field is not a decimal uint32";
    case IRT_POLICY_ERR_RANGE:   return "min_id greater than max_id";
    case IRT_POLICY_ERR_LINELEN: return "line exceeds IRT_POLICY_LINE_MAX";
    default:                     return "unknown policy error";
    }
}

/* 解析十进制 uint32：非空、仅 [0-9]、不溢出。成功返回 0，失败返回 -1。 */
static int parse_u32(const char* s, size_t len, uint32_t* out)
{
    if (len == 0) return -1;

    uint64_t v = 0;
    for (size_t i = 0; i < len; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10u + (uint64_t)(s[i] - '0');
        if (v > 0xFFFFFFFFull) return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* 就地去掉首尾空白（空格 / 制表 / CR / 换行），返回首个非空白字符。 */
static char* trim(char* s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;

    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' ||
            s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = '\0';
    return s;
}

/* 去掉 [s, s+*len) 首尾空白（空格 / 制表），返回新起点并写回新长度。 */
static const char* trim_field(const char* s, size_t* len)
{
    while (*len > 0 && (*s == ' ' || *s == '\t')) { ++s; --*len; }
    while (*len > 0 &&
           (s[*len - 1] == ' ' || s[*len - 1] == '\t')) --*len;
    return s;
}

int irt_policy_load(irt_policy_t* p, const char* path, uint32_t* err_line)
{
    if (err_line) *err_line = 0;
    if (!p || !path) return IRT_POLICY_ERR_ARG;

    FILE* f = fopen(path, "r");
    if (!f) return IRT_POLICY_ERR_OPEN;

    char     line[IRT_POLICY_LINE_MAX];
    uint32_t lineno = 0;
    irt_policy_init(p);

    while (fgets(line, sizeof(line), f)) {
        ++lineno;

        /* fgets 未读到 '\n' 且非文件尾 → 本行超过缓冲区。必须先吞掉本行
         * 剩余字节，否则残段会被当作下一行解析、并产生错位的行号。 */
        int overlong = (!strchr(line, '\n') && !feof(f));

        char* hash = strchr(line, '#');   /* 去注释 */
        if (hash) *hash = '\0';

        char* s = trim(line);

        if (overlong) {
            int c;
            while ((c = fgetc(f)) != EOF && c != '\n') { /* 丢弃残段 */ }
            if (*s != '\0') {             /* 超长且含实质内容（规则）→ 报错 */
                if (err_line) *err_line = lineno;
                fclose(f);
                return IRT_POLICY_ERR_LINELEN;
            }
            continue;                     /* 超长的纯注释 / 空白行 → 容忍 */
        }

        if (*s == '\0') continue;         /* 空行 / 纯注释行 */

        /* 按 ':' 分段并计数（各段首尾空白忽略） */
        const char* fld[4];
        size_t      len[4];
        int         nf  = 0;
        const char* cur = s;
        for (;;) {
            const char* colon = strchr(cur, ':');
            if (nf < 4) {
                size_t flen = colon ? (size_t)(colon - cur) : strlen(cur);
                fld[nf] = trim_field(cur, &flen);
                len[nf] = flen;
            }
            ++nf;
            if (!colon) break;
            cur = colon + 1;
        }

        if (nf != 4) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return IRT_POLICY_ERR_FIELDS;
        }

        uint32_t uid = 0, partition = 0, min_id = 0, max_id = 0;
        int wildcard = (len[1] == 1 && fld[1][0] == '*');

        if (parse_u32(fld[0], len[0], &uid) != 0 ||
            (!wildcard && parse_u32(fld[1], len[1], &partition) != 0) ||
            parse_u32(fld[2], len[2], &min_id) != 0 ||
            parse_u32(fld[3], len[3], &max_id) != 0) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return IRT_POLICY_ERR_NUMBER;
        }

        if (min_id > max_id) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return IRT_POLICY_ERR_RANGE;
        }

        if (p->count >= IRT_POLICY_MAX_RULES) {
            if (err_line) *err_line = lineno;
            fclose(f);
            return IRT_POLICY_ERR_TOOMANY;
        }

        irt_policy_rule_t* r = &p->rules[p->count++];
        r->uid                = uid;
        r->partition          = partition;
        r->min_id             = min_id;
        r->max_id             = max_id;
        r->wildcard_partition = wildcard;
    }

    fclose(f);
    return IRT_POLICY_OK;
}

int irt_policy_allows(const irt_policy_t* p, uint32_t uid,
                      uint32_t partition, uint32_t point_id)
{
    if (!p) return 0;

    for (size_t i = 0; i < p->count; ++i) {
        const irt_policy_rule_t* r = &p->rules[i];
        if (r->uid != uid) continue;
        if (!r->wildcard_partition && r->partition != partition) continue;
        if (point_id < r->min_id) continue;
        if (point_id > r->max_id) continue;
        return 1;
    }
    return 0; /* deny by default */
}
