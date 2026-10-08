/**
 * @file irt_delta.c
 * @brief 运行时点位变更 delta 日志实现（v3.7 主题B B4）
 */
#include "core/irt_delta.h"

#include <internal/irt_types.h>   /* IRT_STATIC_ASSERT */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* 布局锁死：与回放侧逐字节一致 */
IRT_STATIC_ASSERT(sizeof(irt_delta_rec_t) == 80, "irt_delta_rec_t must be 80B");

int irt_delta_open_append(const char* path)
{
    if (!path || path[0] == '\0') return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    return fd;   /* 失败时 -1 */
}

int irt_delta_append(int fd, const irt_delta_rec_t* rec)
{
    if (fd < 0 || !rec) return -1;

    /* 定长单次写：记录 80B 远小于常见页/块大小，单次 write 足够；
     * 失败（含短写）一律视为失败 —— 半条记录比没有记录更危险。 */
    ssize_t n = write(fd, rec, sizeof(*rec));
    if (n != (ssize_t)sizeof(*rec)) return -1;
    if (fsync(fd) != 0) return -1;
    return 0;
}

int irt_delta_replay(const char* path, irt_delta_apply_fn apply, void* ctx)
{
    if (!path || path[0] == '\0' || !apply) return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;   /* 无历史文件：视为空 delta */

    int applied = 0;
    int io_err = 0;      /* 读错误标记：用于与"干净结束"区分（契约要求返回 -1） */
    for (;;) {
        irt_delta_rec_t rec;
        ssize_t got = 0;
        /* 读满一条（可能分多次：普通文件一般一次即可） */
        while (got < (ssize_t)sizeof(rec)) {
            ssize_t k = read(fd, ((char*)&rec) + got, (size_t)(sizeof(rec) - got));
            if (k < 0) {
                /* 读错误：与"文件不存在/无历史"必须区分开，故标记为错误 */
                if (errno == EINTR) continue;
                io_err = 1;
                break;
            }
            if (k == 0) break;                          /* EOF */
            got += k;
        }
        if (io_err) { close(fd); return -1; }
        if (got == 0) break;                            /* 干净结束 */

        if (got != (ssize_t)sizeof(rec)) {
            /* 残片：崩溃时写到一半的最后一条 —— 丢弃它，前面的完整记录仍然有效 */
            close(fd);
            return applied;
        }

        /* 校验：magic / version / op 任一非预期即停止，避免把垃圾当记录解释 */
        if (rec.magic != IRT_DELTA_MAGIC || rec.version != IRT_DELTA_VERSION) {
            close(fd);
            return applied;
        }
        if (rec.op != IRT_DELTA_OP_CREATE && rec.op != IRT_DELTA_OP_DELETE &&
            rec.op != IRT_DELTA_OP_RENAME) {
            close(fd);
            return applied;
        }

        if (apply(&rec, ctx) != 0) { close(fd); return applied; }
        applied++;
    }

    close(fd);
    return applied;
}
