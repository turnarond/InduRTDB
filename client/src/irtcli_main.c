/**
 * @file irtcli_main.c
 * @brief irtcli —— indurtdb-client 的命令行前端（v3.9 T6：分区支持）
 *
 * 语法（选项只在最前，其后为命令流，同一条连接上从左到右顺序执行）：
 *   irtcli [--sock PATH] [--partition N] [--help] ( select N | find NAME | list ) [...]
 *
 * 设计要点：
 *   - 连接惰性建立、之后复用：所有子命令都走 irtcli 库的同步请求-响应通道。
 *   - `--partition N` 等价于在命令流最前插入 `select N`（连接级默认分区）。
 *   - `find` 按当前默认分区查找；`list` 列出当前默认分区点位。
 *   - 失败即按映射码退出（见 cli_to_exit），不继续执行后续命令。
 */
#include <irtcli/client.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IRTCLI_DEFAULT_SOCK "/run/indurtdb/default.sock"
#define IRTCLI_LIST_CAP     4096u   /* list 内部缓冲条数上限（= 传给服务端的 max） */

/* 客户端返回码 → 进程退出码（T6 钉死映射） */
static int cli_to_exit(int rc)
{
    switch (rc) {
    case IRTCLI_OK:            return 0;
    case IRTCLI_ERR_ARG:       return 1;  /* 含用法错误 */
    case IRTCLI_ERR_FULL:      return 2;
    case IRTCLI_ERR_IO:        return 3;
    case IRTCLI_ERR_DENIED:    return 4;
    case IRTCLI_ERR_PROTO:     return 5;
    case IRTCLI_ERR_NOT_FOUND: return 6;
    default:                   return 1;
    }
}

static void print_usage(FILE* out)
{
    fputs(
        "用法: irtcli [--sock PATH] [--partition N] [--help] "
        "(select N | find NAME | list) [...]\n"
        "\n"
        "选项:\n"
        "  --sock PATH     连接的 rtdbd Unix socket（默认 /run/indurtdb/default.sock）\n"
        "  --partition N   连接建立后先选择默认分区 N\n"
        "  --help          显示本帮助并退出\n"
        "\n"
        "命令（按从左到右顺序在同一连接上执行）:\n"
        "  select N        切换本连接默认分区为 N（成功静默）\n"
        "  find NAME       在当前默认分区按名查找，stdout 打印 id\n"
        "  list            列出当前默认分区的全部点位（每行: id partition name）\n",
        out);
}

/* 解析十进制 uint32：严格（须为数值、无尾随字符、不越界）。成功返回 0。 */
static int parse_u32(const char* s, uint32_t* out)
{
    if (!s || *s == '\0') return -1;

    errno = 0;
    char* end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    if (v > 0xFFFFFFFFULL) return -1;

    *out = (uint32_t)v;
    return 0;
}

/* list 子命令：按连接当前默认分区列举并逐行打印。 */
static int do_list(irtcli_t* c)
{
    irtcli_point_info_t* buf =
        (irtcli_point_info_t*)malloc((size_t)IRTCLI_LIST_CAP * sizeof(irtcli_point_info_t));
    if (!buf) return IRTCLI_ERR_IO;

    uint32_t n = 0;
    int rc = irtcli_list_in(c, c->partition, 0, IRTCLI_LIST_CAP,
                            buf, IRTCLI_LIST_CAP, &n);
    if (rc == IRTCLI_OK) {
        for (uint32_t k = 0; k < n; ++k) {
            printf("%u %u %s\n", buf[k].point_id, buf[k].partition, buf[k].name);
        }
    }
    free(buf);
    return rc;
}

int main(int argc, char** argv)
{
    const char* sock = IRTCLI_DEFAULT_SOCK;
    int         have_partition = 0;
    uint32_t    partition = 0;

    /* ---- 选项阶段：只识别最前的选项；遇到首个非选项即进入命令流 ---- */
    int i = 1;
    for (; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--help") == 0) {
            print_usage(stdout);
            return 0;
        } else if (strcmp(a, "--sock") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "irtcli: --sock 缺少参数\n");
                print_usage(stderr);
                return 1;
            }
            sock = argv[++i];
        } else if (strcmp(a, "--partition") == 0) {
            if (i + 1 >= argc || parse_u32(argv[i + 1], &partition) != 0) {
                fprintf(stderr, "irtcli: --partition 需要一个 uint32 参数\n");
                print_usage(stderr);
                return 1;
            }
            ++i;
            have_partition = 1;
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "irtcli: 未知选项: %s\n", a);
            print_usage(stderr);
            return 1;
        } else {
            break;  /* 命令流开始 */
        }
    }

    if (i >= argc) {
        fprintf(stderr, "irtcli: 缺少命令\n");
        print_usage(stderr);
        return 1;
    }

    irtcli_t c;
    int rc = irtcli_init(&c, sock, 0, NULL, NULL);
    if (rc != IRTCLI_OK) {
        fprintf(stderr, "irtcli: 初始化失败\n");
        return cli_to_exit(rc);
    }

    /* `--partition N`：先选择连接默认分区，失败即按码退出。 */
    if (have_partition) {
        rc = irtcli_select(&c, partition);
        if (rc != IRTCLI_OK) {
            fprintf(stderr, "irtcli: select %u 失败\n", partition);
            irtcli_close(&c);
            return cli_to_exit(rc);
        }
    }

    /* ---- 命令流：同一条连接顺序执行 ---- */
    for (; i < argc; ++i) {
        const char* cmd = argv[i];

        if (strcmp(cmd, "select") == 0) {
            uint32_t p = 0;
            if (i + 1 >= argc || parse_u32(argv[i + 1], &p) != 0) {
                fprintf(stderr, "irtcli: select 需要一个 uint32 分区参数\n");
                print_usage(stderr);
                irtcli_close(&c);
                return 1;
            }
            ++i;
            rc = irtcli_select(&c, p);
            if (rc != IRTCLI_OK) {
                fprintf(stderr, "irtcli: select %u 失败\n", p);
                irtcli_close(&c);
                return cli_to_exit(rc);
            }
            /* 成功静默：不打印，成功即更新后续命令的默认分区 */
        } else if (strcmp(cmd, "find") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "irtcli: find 需要一个点位名参数\n");
                print_usage(stderr);
                irtcli_close(&c);
                return 1;
            }
            const char* name = argv[++i];
            uint32_t    id   = 0;
            rc = irtcli_find_by_name(&c, name, &id);
            if (rc == IRTCLI_ERR_NOT_FOUND) {
                fprintf(stderr, "not found: %s\n", name);
                irtcli_close(&c);
                return cli_to_exit(rc);  /* 6 */
            }
            if (rc != IRTCLI_OK) {
                fprintf(stderr, "irtcli: find %s 失败\n", name);
                irtcli_close(&c);
                return cli_to_exit(rc);
            }
            printf("%u\n", id);
        } else if (strcmp(cmd, "list") == 0) {
            rc = do_list(&c);
            if (rc != IRTCLI_OK) {
                fprintf(stderr, "irtcli: list 失败\n");
                irtcli_close(&c);
                return cli_to_exit(rc);
            }
        } else {
            fprintf(stderr, "irtcli: 未知命令: %s\n", cmd);
            print_usage(stderr);
            irtcli_close(&c);
            return 1;
        }
    }

    irtcli_close(&c);
    return 0;
}
