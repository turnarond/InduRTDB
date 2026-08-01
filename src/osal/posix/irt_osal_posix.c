/**
 * @file irt_osal_posix.c
 * @brief POSIX OSAL 实现 (直译自 time_posix.cpp / shared_memory_posix.cpp)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "osal/irt_osal.h"

#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>

uint64_t irt_time_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

void irt_time_sleep_ns(uint64_t duration_ns) {
    struct timespec req, rem;
    req.tv_sec  = (time_t)(duration_ns / 1000000000ULL);
    req.tv_nsec = (long)(duration_ns % 1000000000ULL);
    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

void* irt_shm_os_map(irt_shm_os_t* s, const char* name, size_t size) {
    if (!s || !name || name[0] == '\0' || size == 0) return NULL;
    if (s->mapped) return s->mapped;

    int n = snprintf(s->name, sizeof(s->name), "%s", name);
    if (n < 0 || (size_t)n >= sizeof(s->name)) return NULL;
    s->size = size;

    /* O_EXCL: 原子判断创建者, 不依赖 st_size==0 (残留段会干扰) */
    s->fd = shm_open(s->name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (s->fd >= 0) {
        s->owner = true;
    } else if (errno == EEXIST) {
        s->fd = shm_open(s->name, O_RDWR, 0666);
        if (s->fd < 0) return NULL;
    } else {
        return NULL;
    }

    if (s->owner && ftruncate(s->fd, (off_t)size) < 0) {
        close(s->fd); s->fd = -1;
        shm_unlink(s->name);
        return NULL;
    }

    s->mapped = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->mapped == MAP_FAILED) {
        close(s->fd); s->fd = -1;
        s->mapped = NULL;
        if (s->owner) shm_unlink(s->name);
        return NULL;
    }
    return s->mapped;
}

void irt_shm_os_unmap(irt_shm_os_t* s) {
    if (!s) return;
    if (s->mapped && s->size > 0) {
        munmap(s->mapped, s->size);
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    if (s->owner) {
        shm_unlink(s->name);
    }
    s->mapped = NULL;
    s->size = 0;
    s->owner = false;
}

bool irt_shm_os_is_owner(const irt_shm_os_t* s) {
    return s ? s->owner : false;
}

void irt_shm_os_claim_ownership(irt_shm_os_t* s) {
    if (s && !s->owner) {
        s->owner = true;  /* 接管所有权: 后续 shm_unlink 由本进程负责 */
    }
}
