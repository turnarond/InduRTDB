/**
 * @file logbuf.h
 * @brief 运行日志：定长环形缓冲（无堆分配）
 *
 * 记录 rtdbd 自身运行事件（启动/监听/配置加载/管控写/worker 重启/停止等），
 * 供监控台「运行日志」页面与终端 `logs` 命令读取（OP_GET_LOG）。
 *
 * 与审计(audit)的区别：audit 记「谁在何时写了哪个点」；logbuf 记进程自身运行事件。
 *
 * 并发：rtdbd 为单线程串行处理（poll 循环），无需加锁。
 */
#ifndef RTDBD_LOGBUF_H_
#define RTDBD_LOGBUF_H_

#include <rtdbd/protocol.h>
#include <stdint.h>

typedef struct {
    rtdbd_log_entry_t entries[RTDBD_LOG_CAPACITY];
    uint32_t          head;  /* 下一个写入位置 */
    uint32_t          count; /* 当前有效条目数（上限 RTDBD_LOG_CAPACITY） */
} irt_logbuf_t;

void irt_logbuf_init(irt_logbuf_t* b);

/* 记录一条日志：同时输出到 stderr/stdout（保持既有控制台行为）并入环形缓冲。
 * msg 超过 RTDBD_LOG_ENTRY_MSG_MAX-1 会被截断。 */
void irt_logbuf_emit(irt_logbuf_t* b, uint8_t level, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* 按最旧→最新顺序导出，最多 cap 条；返回实际导出条数 */
uint32_t irt_logbuf_dump(const irt_logbuf_t* b, rtdbd_log_entry_t* out, uint32_t cap);

#endif /* RTDBD_LOGBUF_H_ */
