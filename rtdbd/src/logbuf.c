/**
 * @file logbuf.c
 * @brief 运行日志环形缓冲实现（定长、无堆分配）
 *
 * 时间戳取 CLOCK_REALTIME（墙上时钟），便于监控台直接展示可读时间；
 * 与审计/统计用的 CLOCK_MONOTONIC 区分，二者语义不同，不可混用。
 */
#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void irt_logbuf_init(irt_logbuf_t* b)
{
    if (!b) return;
    memset(b, 0, sizeof(*b));
}

static uint64_t now_wall_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void irt_logbuf_emit(irt_logbuf_t* b, uint8_t level, const char* fmt, ...)
{
    char msg[RTDBD_LOG_ENTRY_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    msg[sizeof(msg) - 1] = '\0';

    /* 保持既有控制台行为：ERROR 走 stderr，其余走 stdout */
    FILE* out = (level == RTDBD_LOG_ERROR) ? stderr : stdout;
    fprintf(out, "%s\n", msg);
    fflush(out);

    if (!b) return;

    rtdbd_log_entry_t* e = &b->entries[b->head];
    memset(e, 0, sizeof(*e));
    e->ts_ns = now_wall_ns();
    e->level = level;
    memcpy(e->msg, msg, sizeof(e->msg));
    e->msg[sizeof(e->msg) - 1] = '\0';

    b->head = (b->head + 1u) % RTDBD_LOG_CAPACITY;
    if (b->count < RTDBD_LOG_CAPACITY) b->count++;
}

uint32_t irt_logbuf_dump(const irt_logbuf_t* b, rtdbd_log_entry_t* out, uint32_t cap)
{
    if (!b || !out || cap == 0) return 0;

    uint32_t n = (b->count < cap) ? b->count : cap;
    /* 最旧 → 最新：环形起点为 count 未满时的 0，满时为 head */
    uint32_t start = (b->count == RTDBD_LOG_CAPACITY)
                         ? b->head
                         : 0u;
    for (uint32_t i = 0; i < n; ++i) {
        const rtdbd_log_entry_t* src = &b->entries[(start + i) % RTDBD_LOG_CAPACITY];
        memcpy(&out[i], src, sizeof(*src));
    }
    return n;
}
