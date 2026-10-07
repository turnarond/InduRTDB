/**
 * @file smoke_leak.c
 * @brief T8 诊断工具 C harness：冒烟（init→write→read→subscribe→shutdown）
 *        与 fd 泄漏检测（反复启停）。供 irt-diag 的 smoke/leak 命令驱动。
 *
 * 仅用公共 C API 与 POSIX，不引入 C++。每个步骤打印 SMOKE_* 标记，便于
 * 测试断言；泄漏模式打印 LEAK_INIT_FD / LEAK_FINAL_FD 供 fd 增量判定。
 */
#include <indurtdb/indurtdb.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

static void on_point(uint32_t id, const indurtdb_point_t* data, void* ud) {
    (void)id; (void)data; (void)ud;
}

static int count_fds(void) {
    DIR* d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        ++n;
    }
    closedir(d);
    return n;
}

static void cleanup(const char* id) {
    char name[256];
    snprintf(name, sizeof(name), "/indurtdb_%s", id);
    shm_unlink(name);  /* 忽略失败：可能不存在 */
}

static void usage(const char* prog) {
    fprintf(stderr,
        "usage: %s --smoke|--leak [--id ID] [--points N] [--subs M] [--cycles C]\n",
        prog);
}

int main(int argc, char** argv) {
    const char* mode = NULL;
    const char* id = "diag_smoke";
    int points = 64, subs = 4, cycles = 20;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--smoke") == 0) mode = "smoke";
        else if (strcmp(argv[i], "--leak") == 0) mode = "leak";
        else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) id = argv[++i];
        else if (strcmp(argv[i], "--points") == 0 && i + 1 < argc) points = atoi(argv[++i]);
        else if (strcmp(argv[i], "--subs") == 0 && i + 1 < argc) subs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) cycles = atoi(argv[++i]);
        else { usage(argv[0]); return 2; }
    }
    if (!mode) { usage(argv[0]); return 2; }

    cleanup(id);

    if (strcmp(mode, "smoke") == 0) {
        if (indurtdb_initialize(id, (uint32_t)points, (uint32_t)subs) != 0) {
            printf("SMOKE_FAIL step=init\n"); return 1;
        }
        printf("SMOKE_INIT ok\n");

        if (indurtdb_write_int32(1, 42) != 0) {
            printf("SMOKE_FAIL step=write\n"); indurtdb_shutdown(); return 1;
        }
        printf("SMOKE_WRITE ok\n");

        int32_t v = 0;
        if (indurtdb_read_int32(1, &v) != 0 || v != 42) {
            printf("SMOKE_FAIL step=read\n"); indurtdb_shutdown(); return 1;
        }
        printf("SMOKE_READ ok value=%d\n", (int)v);

        if (indurtdb_subscribe(1, on_point, NULL) != 0) {
            printf("SMOKE_FAIL step=subscribe\n"); indurtdb_shutdown(); return 1;
        }
        printf("SMOKE_SUB ok\n");

        if (indurtdb_unsubscribe(1) != 0) {
            printf("SMOKE_FAIL step=unsubscribe\n"); indurtdb_shutdown(); return 1;
        }
        printf("SMOKE_UNSUB ok\n");

        indurtdb_shutdown();
        printf("SMOKE_SHUTDOWN ok\n");
        return 0;
    }

    /* leak：反复启停，比较 fd 计数 */
    int init_fd = count_fds();
    printf("LEAK_INIT_FD %d\n", init_fd);
    for (int c = 0; c < cycles; ++c) {
        if (indurtdb_initialize(id, (uint32_t)points, (uint32_t)subs) != 0) {
            printf("LEAK_FAIL step=init cycle=%d\n", c); return 1;
        }
        indurtdb_shutdown();
    }
    int final_fd = count_fds();
    printf("LEAK_FINAL_FD %d\n", final_fd);
    return 0;
}
