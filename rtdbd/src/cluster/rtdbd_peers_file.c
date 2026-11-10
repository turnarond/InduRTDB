/**
 * @file rtdbd_peers_file.c
 * @brief v3.12 M3.3 切片 E：从 `--peers-file` 加载**初始成员表**（配置来源，非运行时状态）。
 *
 * 纯逻辑模块：不依赖 fd / socket / VSOA / rtdbd 全局状态，故默认构建即可单测、
 * 不膨胀 rtdbd.c（codegraph 评审：rtdbd.c 已是上帝对象，新逻辑拆出独立文件）。
 * 文件格式：每行 `<addr> <node_id>`，`#` 注释、空行忽略；上限 RTDBD_PEER_MAX。
 * 加载走与运行态 SET_PEERS 相同的 `rtdbd_peers_apply_set()`（整组替换 + 自地址拒绝 +
 * diff 入延迟队列），保证「配置来源」与「运行态管控」语义一致。
 * 运行态 admin SET_PEERS 会整组覆盖本表；peers-file 仅提供重启后初始成员，不回写。
 */
#include "cluster/rtdbd_peers_global.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

int rtdbd_peers_load_file(const char *path)
{
    if (!path || !path[0]) {
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    rtdbd_peer_t buf[RTDBD_PEER_MAX];
    uint32_t n = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        /* 去注释 */
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        /* 首 token = addr，次 token = node_id */
        char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        if (*p == '\0') {
            continue; /* 空行 / 纯注释行 */
        }
        char *addr = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        if (*p) {
            *p++ = '\0';
        }
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        char *node = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        *p = '\0';

        if (n >= RTDBD_PEER_MAX) {
            fclose(f);
            return -1; /* 超出成员上限 */
        }
        memset(&buf[n], 0, sizeof(buf[n]));
        size_t la = strlen(addr);
        size_t ln = strlen(node);
        size_t ca = (la < sizeof(buf[n].addr) - 1) ? la : (sizeof(buf[n].addr) - 1);
        size_t cn = (ln < sizeof(buf[n].node_id) - 1) ? ln : (sizeof(buf[n].node_id) - 1);
        memcpy(buf[n].addr, addr, ca);       /* buf 已 memset 0，null 终止保证 */
        memcpy(buf[n].node_id, node, cn);
        buf[n].state = RTDBD_PEER_ST_UNKNOWN; /* 终态由循环尾延迟动作落地 */
        n++;
    }
    fclose(f);

    if (n == 0) {
        return 0; /* 空文件合法：仅不预置成员 */
    }
    return rtdbd_peers_apply_set(buf, n);
}
