/**
 * @file irt_osal.h
 * @brief OS 抽象层 (无虚表; 平台实现由构建系统选择)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_OSAL_IRT_OSAL_H_
#define IRT_OSAL_IRT_OSAL_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 时间 ---- */
uint64_t irt_time_now_ns(void);
void     irt_time_sleep_ns(uint64_t duration_ns);

/* ---- 共享内存 ---- */
typedef struct {
    char   name[64];
    int    fd;
    void*  mapped;
    size_t size;
    bool   owner;
} irt_shm_os_t;

/* 映射成功返回基址, 失败返回 NULL. owner 检测通过 O_EXCL 原子判断 */
void* irt_shm_os_map(irt_shm_os_t* s, const char* name, size_t size);
/* munmap + close + (owner ? shm_unlink : 0), 清零 struct */
void  irt_shm_os_unmap(irt_shm_os_t* s);
bool  irt_shm_os_is_owner(const irt_shm_os_t* s);
void  irt_shm_os_claim_ownership(irt_shm_os_t* s);  /* v3.1: attacher 接管崩溃 owner 的所有权 */

#ifdef __cplusplus
}
#endif

#endif /* IRT_OSAL_IRT_OSAL_H_ */
