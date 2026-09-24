#include "policy.h"

#include <stdio.h>
#include <string.h>

void irt_policy_init(irt_policy_t* p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

int irt_policy_load(irt_policy_t* p, const char* path)
{
    if (!p || !path) return -1;

    FILE* f = fopen(path, "r");
    if (!f) return -2;

    char line[IRT_POLICY_LINE_MAX];
    irt_policy_init(p);

    while (fgets(line, sizeof(line), f)) {
        /* 去注释与行尾 */
        char* hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        unsigned long uid = 0, min_id = 0, max_id = 0;
        if (sscanf(line, "%lu:%lu:%lu", &uid, &min_id, &max_id) != 3) {
            continue; /* 空行或格式不符，跳过 */
        }
        if (p->count >= IRT_POLICY_MAX_RULES) {
            fclose(f);
            return -3; /* 规则数超限 */
        }
        p->rules[p->count].uid    = (uint32_t)uid;
        p->rules[p->count].min_id = (uint32_t)min_id;
        p->rules[p->count].max_id = (uint32_t)max_id;
        p->count++;
    }

    fclose(f);
    return 0;
}

int irt_policy_allows(const irt_policy_t* p, uint32_t uid, uint32_t point_id)
{
    if (!p) return 0;
    for (size_t i = 0; i < p->count; ++i) {
        if (p->rules[i].uid != uid) continue;
        if (point_id < p->rules[i].min_id) continue;
        if (point_id > p->rules[i].max_id) continue;
        return 1;
    }
    return 0; /* deny by default */
}
