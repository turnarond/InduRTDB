/**
 * @file audit.h
 * @brief 写操作审计：定长环形缓冲（无堆分配）
 *
 * 记录每次成功写入的 pid / uid / point_id / ts，供事后追溯"谁在何时写了哪个点"。
 */
#ifndef RTDBD_AUDIT_H_
#define RTDBD_AUDIT_H_

#include <rtdbd/protocol.h>
#include <stdint.h>

typedef struct {
    rtdbd_audit_entry_t entries[RTDBD_AUDIT_CAPACITY];
    uint32_t            head;  /* 下一个写入位置 */
    uint32_t            count; /* 当前有效条目数（上限 RTDBD_AUDIT_CAPACITY） */
} irt_audit_t;

void     irt_audit_init(irt_audit_t* a);
void     irt_audit_record(irt_audit_t* a, uint32_t pid, uint32_t uid,
                          uint32_t point_id, uint64_t ts_ns);
/* 按最旧→最新顺序导出，返回实际导出条数 */
uint32_t irt_audit_dump(const irt_audit_t* a, rtdbd_audit_entry_t* out, uint32_t cap);

#endif /* RTDBD_AUDIT_H_ */
