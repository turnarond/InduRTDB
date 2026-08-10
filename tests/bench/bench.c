/**
 * @file bench.c
 * @brief InduRTDB 性能基准测试程序 (x86)
 * @version 3.1.0
 * @date 2026-08-10
 * @copyright MIT License
 *
 * 测量 InduRTDB 核心操作的延迟分布 (P50/P99/P99.9/min/max/mean)
 * 与吞吐量, 用于产品化交付的性能验证.
 *
 * 被测操作:
 *   - 单点写: write_int32, write_double
 *   - 单点读: read_int32, read_double
 *   - 快速读: peek (seqlock 单拷贝, 零分配)
 *   - 批量读: read_range
 *   - 批量写: write_range_int32
 *   - 混合吞吐: write + read 交替, 计次统计 ops/sec
 *
 * 计时: clock_gettime(CLOCK_MONOTONIC), 纳秒精度.
 * 参数可通过编译宏或命令行覆盖.
 *
 * 用法:
 *   bench [-w 预热次数] [-i 迭代次数] [-b 批量点数] [-t 吞吐秒数] [-h]
 */

#include <indurtdb/indurtdb.h>

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ================================================================
 * 编译期默认参数 (可通过 -D 覆盖)
 * ================================================================ */

#ifndef BENCH_WARMUP
#define BENCH_WARMUP 20000 /* 默认预热迭代次数 */
#endif

#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000 /* 默认测量迭代次数 */
#endif

#ifndef BENCH_BATCH_ITERS
#define BENCH_BATCH_ITERS 50000 /* 批量操作迭代次数 */
#endif

#ifndef BENCH_BATCH_MAX
#define BENCH_BATCH_MAX 1024 /* 批量操作缓冲区上限 */
#endif

#ifndef BENCH_BATCH_COUNT
#define BENCH_BATCH_COUNT 100 /* 默认单次批量操作点数 */
#endif

#ifndef BENCH_MAX_POINTS
#define BENCH_MAX_POINTS 4096 /* 共享内存最大点数 */
#endif

#ifndef BENCH_THROUGHPUT_SEC
#define BENCH_THROUGHPUT_SEC 2 /* 吞吐测试时长 (秒) */
#endif

#define INSTANCE_ID "bench"

/* ================================================================
 * 计时辅助
 * ================================================================ */

/** 获取单调时钟, 单位纳秒 */
static inline uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* ================================================================
 * 统计结果
 * ================================================================ */

/** 单次基准的统计结果 */
typedef struct {
  const char *name;   /* 操作名称 */
  double min_us;      /* 最小延迟 (微秒) */
  double max_us;      /* 最大延迟 (微秒) */
  double mean_us;     /* 平均延迟 (微秒) */
  double p50_us;      /* P50 延迟 (微秒) */
  double p99_us;      /* P99 延迟 (微秒) */
  double p999_us;     /* P99.9 延迟 (微秒) */
  double throughput;  /* 吞吐 (op/s 或 point/s) */
  int iterations;     /* 实际迭代次数 */
} bench_result_t;

/* ---- 排序比较 (qsort) ---- */
static int cmp_double(const void *a, const void *b) {
  double da = *(const double *)a;
  double db = *(const double *)b;
  if (da < db) return -1;
  if (da > db) return 1;
  return 0;
}

/** 对已采集的延迟数组计算统计量, 结果写入 r (纳秒->微秒) */
static void compute_stats(double *lats, int count, bench_result_t *r) {
  if (count <= 0) {
    memset(r, 0, sizeof(*r));
    return;
  }

  qsort(lats, (size_t)count, sizeof(double), cmp_double);

  r->min_us = lats[0] / 1000.0;
  r->max_us = lats[count - 1] / 1000.0;
  r->p50_us = lats[count / 2] / 1000.0;
  r->p99_us = lats[(int)(count * 0.99)] / 1000.0;
  r->p999_us = lats[(int)(count * 0.999)] / 1000.0;

  double sum = 0.0;
  for (int i = 0; i < count; i++) sum += lats[i];
  r->mean_us = (sum / (double)count) / 1000.0;
  r->iterations = count;
}

/* ================================================================
 * 全局状态 (供各操作闭包使用)
 * ================================================================ */

static double *g_latencies = NULL; /* 延迟采集数组 */
static int g_lat_cap = 0;          /* 数组容量 */
static int g_lat_cnt = 0;          /* 当前已采集数 */

/* 防止编译器将读/peek 结果优化掉 */
static volatile int32_t g_sink_i32;
static volatile double g_sink_dbl;
static volatile const indurtdb_point_t *g_sink_pt;

/* 批量操作缓冲区 */
static int32_t g_batch_wbuf[BENCH_BATCH_MAX];
static indurtdb_point_t g_batch_rbuf[BENCH_BATCH_MAX];

/* 运行时批量点数 (可被 -b 覆盖) */
static int g_batch_cnt = BENCH_BATCH_COUNT;

/* 混合读写吞吐用的点位数 */
#define MIXED_COUNT 64
#define MIXED_START 2000

/* ================================================================
 * 基准运行引擎
 * ================================================================ */

/** 执行一次基准: 先预热 warmup 次, 再测量 iters 次, 记录每次耗时 */
static void bench_run(void (*op)(void), int warmup, int iters) {
  g_lat_cnt = 0;
  for (int i = 0; i < warmup; i++) op();
  for (int i = 0; i < iters; i++) {
    uint64_t t0 = now_ns();
    op();
    uint64_t t1 = now_ns();
    g_latencies[g_lat_cnt++] = (double)(t1 - t0);
  }
}

/* ================================================================
 * 各被测操作的闭包函数
 * ================================================================ */

/* ---- 单点写 int32 (id=0) ---- */
static void op_write_i32(void) {
  static int v = 0;
  (void)indurtdb_write_int32(0, (int32_t)v);
  v = (v + 1) % 10000;
}

/* ---- 单点写 double (id=1) ---- */
static void op_write_dbl(void) {
  static double v = 0.0;
  (void)indurtdb_write_double(1, v);
  v += 1.0;
}

/* ---- 单点读 int32 (id=0) ---- */
static void op_read_i32(void) {
  int32_t val;
  (void)indurtdb_read_int32(0, &val);
  g_sink_i32 = val;
}

/* ---- 单点读 double (id=1) ---- */
static void op_read_dbl(void) {
  double val;
  (void)indurtdb_read_double(1, &val);
  g_sink_dbl = val;
}

/* ---- peek (id=2, seqlock 单拷贝快速读) ---- */
static void op_peek(void) {
  g_sink_pt = indurtdb_peek(2);
}

/* ---- 批量读 read_range (start=10, count=g_batch_cnt) ---- */
static void op_read_range(void) {
  (void)indurtdb_read_range(10, (uint16_t)g_batch_cnt, g_batch_rbuf,
                             (uint16_t)BENCH_BATCH_MAX);
  g_sink_pt = &g_batch_rbuf[0];
}

/* ---- 批量写 write_range_int32 (start=10+BENCH_BATCH_MAX, count=g_batch_cnt) ---- */
static void op_write_range(void) {
  static int tick = 0;
  for (int i = 0; i < g_batch_cnt; i++) {
    g_batch_wbuf[i] = tick + i;
  }
  tick++;
  (void)indurtdb_write_range_int32(10 + BENCH_BATCH_MAX, g_batch_wbuf,
                                    (uint16_t)g_batch_cnt);
}

/* ---- 混合读写 (写一个点 + 读一个点) ---- */
static void op_mixed(void) {
  static int tick = 0;
  int i = tick % MIXED_COUNT;
  (void)indurtdb_write_int32(MIXED_START + i, (int32_t)tick);
  int j = (tick + 1) % MIXED_COUNT;
  int32_t val;
  (void)indurtdb_read_int32(MIXED_START + j, &val);
  g_sink_i32 = val;
  tick++;
}

/* ================================================================
 * 结果注册与执行
 * ================================================================ */

#define MAX_RESULTS 16
static bench_result_t g_results[MAX_RESULTS];
static int g_result_cnt = 0;

/** 注册并运行一次延迟分布基准 */
static void add_latency_bench(const char *name, void (*op)(void), int warmup,
                               int iters, double batch_factor) {
  bench_result_t *r = &g_results[g_result_cnt++];
  memset(r, 0, sizeof(*r));
  r->name = name;

  bench_run(op, warmup, iters);
  compute_stats(g_latencies, g_lat_cnt, r);

  /* 吞吐 = 点数/秒 (单点操作 batch_factor=1, 批量=batch_cnt) */
  if (r->mean_us > 0.0) {
    r->throughput = (batch_factor * 1e6) / r->mean_us;
  }
}

/** 注册并运行一次吞吐基准 (基于固定时长的计次) */
static void add_throughput_bench(const char *name, void (*op)(void),
                                  int duration_sec, double ops_per_call) {
  bench_result_t *r = &g_results[g_result_cnt++];
  memset(r, 0, sizeof(*r));
  r->name = name;

  /* 短暂预热 */
  uint64_t warmup_end =
      now_ns() + (uint64_t)(duration_sec / 2) * UINT64_C(1000000000);
  while (now_ns() < warmup_end) op();

  /* 正式计次 */
  int64_t count = 0;
  uint64_t t_end =
      now_ns() + (uint64_t)duration_sec * UINT64_C(1000000000);
  while (now_ns() < t_end) {
    op();
    count++;
  }

  r->throughput = (double)count * ops_per_call / (double)duration_sec;
  r->iterations = (int)count;
}

/* ================================================================
 * 输出函数
 * ================================================================ */

static void print_header(void) {
  printf("\n");
  printf("+==============================================================================+\n");
  printf("|          InduRTDB 性能基准测试报告 (x86, Release -O2)                       |\n");
  printf("+==============================================================================+\n");
}

static void print_table(void) {
  printf("| %-24s | %8s | %8s | %8s | %8s | %8s | %8s | %12s |\n",
         "操作", "迭代数", "P50(us)", "P99(us)", "P999(us)", "Min(us)",
         "Max(us)", "吞吐(op/s)");
  printf("|--------------------------+----------+----------+----------"
         "+----------+----------+----------+--------------|\n");

  for (int i = 0; i < g_result_cnt; i++) {
    bench_result_t *r = &g_results[i];
    if (r->p50_us == 0.0 && r->p99_us == 0.0 && r->p999_us == 0.0) {
      /* 纯吞吐行 (无延迟分布) */
      printf("| %-24s | %8d |        - |        - |        - |        - "
             "|        - | %12.0f |\n",
             r->name, r->iterations, r->throughput);
    } else {
      printf("| %-24s | %8d | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f "
             "| %12.0f |\n",
             r->name, r->iterations, r->p50_us, r->p99_us, r->p999_us,
             r->min_us, r->max_us, r->throughput);
    }
  }

  printf("+--------------------------+----------+----------+----------"
         "+----------+----------+----------+--------------+\n");
}

static void print_target_comparison(void) {
  /* 查找关键操作的 P99 */
  double w_i32_p99 = -1.0, r_i32_p99 = -1.0, pk_p99 = -1.0;
  double w_dbl_p99 = -1.0, r_dbl_p99 = -1.0;
  double rr_p99 = -1.0, wr_p99 = -1.0;

  for (int i = 0; i < g_result_cnt; i++) {
    bench_result_t *r = &g_results[i];
    if (strcmp(r->name, "write_int32") == 0) w_i32_p99 = r->p99_us;
    if (strcmp(r->name, "read_int32") == 0) r_i32_p99 = r->p99_us;
    if (strcmp(r->name, "peek") == 0) pk_p99 = r->p99_us;
    if (strcmp(r->name, "write_double") == 0) w_dbl_p99 = r->p99_us;
    if (strcmp(r->name, "read_double") == 0) r_dbl_p99 = r->p99_us;
    if (strcmp(r->name, "read_range") == 0) rr_p99 = r->p99_us;
    if (strcmp(r->name, "write_range_int32") == 0) wr_p99 = r->p99_us;
  }

  printf("\n");
  printf("+=======================================================================+\n");
  printf("|                    设计目标 vs 实测对照 (x86)                         |\n");
  printf("+----------------------------------+-------------+-----------+----------+\n");
  printf("| 指标                             | 设计目标    | 实测值    | 判定     |\n");
  printf("+----------------------------------+-------------+-----------+----------+\n");

#define CMP_ROW(label, target, measured)                                     \
  do {                                                                       \
    const char *verdict =                                                     \
        ((measured) < 0.0) ? "N/A"                                           \
        : ((measured) <= (target)) ? "PASS"                                  \
                                   : "FAIL";                                 \
    printf("| %-32s | %8.1f us  | ", label, (double)(target));             \
    if ((measured) < 0.0)                                                    \
      printf("    N/A    | %-8s |\n", verdict);                              \
    else                                                                     \
      printf("%8.3f us | %-8s |\n", (measured), verdict);                    \
  } while (0)

  CMP_ROW("P99 write_int32", 10.0, w_i32_p99);
  CMP_ROW("P99 write_double", 10.0, w_dbl_p99);
  CMP_ROW("P99 read_int32", 10.0, r_i32_p99);
  CMP_ROW("P99 read_double", 10.0, r_dbl_p99);
  CMP_ROW("P99 peek", 10.0, pk_p99);
  CMP_ROW("P99 read_range (单点均摊)", 10.0,
          (rr_p99 > 0.0) ? rr_p99 / g_batch_cnt : -1.0);
  CMP_ROW("P99 write_range (单点均摊)", 10.0,
          (wr_p99 > 0.0) ? wr_p99 / g_batch_cnt : -1.0);

  printf("+----------------------------------+-------------+-----------+----------+\n");
  printf("| 注: ARM 平台实测待硬件到位后补充.                                  |\n");
  printf("+----------------------------------+-------------+-----------+----------+\n");
  printf("\n");
}

/* ================================================================
 * 入口
 * ================================================================ */

static void show_usage(const char *prog) {
  printf("用法: %s [选项]\n", prog);
  printf("选项:\n");
  printf("  -w N      预热迭代次数       (默认 %d)\n", BENCH_WARMUP);
  printf("  -i N      测量迭代次数       (默认 %d)\n", BENCH_ITERATIONS);
  printf("  -b N      单次批量操作点数   (默认 %d, 最大 %d)\n", BENCH_BATCH_COUNT,
         BENCH_BATCH_MAX);
  printf("  -t N      吞吐测试时长 (秒)  (默认 %d)\n", BENCH_THROUGHPUT_SEC);
  printf("  -h        显示此帮助信息\n");
}

int main(int argc, char **argv) {
  int warmup = BENCH_WARMUP;
  int iters = BENCH_ITERATIONS;
  int tp_sec = BENCH_THROUGHPUT_SEC;

  /* ---- 解析命令行参数 ---- */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0) {
      show_usage(argv[0]);
      return 0;
    } else if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
      warmup = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
      iters = atoi(argv[++i]);
    } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
      g_batch_cnt = atoi(argv[++i]);
      if (g_batch_cnt < 1 || g_batch_cnt > BENCH_BATCH_MAX) {
        fprintf(stderr, "批量点数需在 [1, %d] 范围内, 得到 %d\n",
                BENCH_BATCH_MAX, g_batch_cnt);
        return 1;
      }
    } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
      tp_sec = atoi(argv[++i]);
    } else {
      fprintf(stderr, "未知选项: %s\n", argv[i]);
      show_usage(argv[0]);
      return 1;
    }
  }

  int batch_iters = BENCH_BATCH_ITERS;
  if (batch_iters > iters) batch_iters = iters;

  /* ---- 分配延迟采集数组 ---- */
  int max_iters = iters;
  g_latencies = (double *)malloc((size_t)max_iters * sizeof(double));
  if (!g_latencies) {
    fprintf(stderr, "malloc 失败\n");
    return 1;
  }
  g_lat_cap = max_iters;

  /* ---- 初始化 InduRTDB ---- */
  printf("InduRTDB 性能基准测试 v" INDURTDB_VERSION_STRING "\n");
  printf("参数: 预热=%d  迭代=%d  批量点数=%d  吞吐时长=%ds\n\n",
         warmup, iters, g_batch_cnt, tp_sec);

  if (indurtdb_initialize(INSTANCE_ID, BENCH_MAX_POINTS, 16) != 0) {
    fprintf(stderr, "初始化失败: %s\n", indurtdb_get_last_error());
    free(g_latencies);
    return 1;
  }

  /* ---- 预填点位数据 (确保读/peek 操作命中已有数据) ---- */
  indurtdb_write_int32(0, 42);
  indurtdb_write_double(1, 3.14);
  indurtdb_write_int32(2, 99);
  for (int i = 0; i < g_batch_cnt; i++) {
    indurtdb_write_int32(10 + i, 100 + i);
  }
  for (int i = 0; i < g_batch_cnt; i++) {
    indurtdb_write_int32(10 + BENCH_BATCH_MAX + i, 0);
  }
  for (int i = 0; i < MIXED_COUNT; i++) {
    indurtdb_write_int32(MIXED_START + i, 0);
  }
  for (int i = 0; i < g_batch_cnt; i++) {
    g_batch_wbuf[i] = i;
  }

  printf("点位预填完成, 开始基准测试...\n");

  /* ================================================================
   * 运行全部基准
   * ================================================================ */

  /* -- 单点写 -- */
  add_latency_bench("write_int32", op_write_i32, warmup, iters, 1.0);
  add_latency_bench("write_double", op_write_dbl, warmup, iters, 1.0);

  /* -- 单点读 -- */
  add_latency_bench("read_int32", op_read_i32, warmup, iters, 1.0);
  add_latency_bench("read_double", op_read_dbl, warmup, iters, 1.0);

  /* -- peek (快速读) -- */
  add_latency_bench("peek", op_peek, warmup, iters, 1.0);

  /* -- 批量 -- */
  add_latency_bench("read_range", op_read_range, warmup, batch_iters,
                    (double)g_batch_cnt);
  add_latency_bench("write_range_int32", op_write_range, warmup, batch_iters,
                    (double)g_batch_cnt);

  /* -- 混合吞吐 -- */
  add_throughput_bench("mixed_rw (吞吐)", op_mixed, tp_sec, 2.0);

  /* ================================================================
   * 输出报告
   * ================================================================ */

  print_header();
  print_table();
  print_target_comparison();

  /* ---- 清理 ---- */
  indurtdb_shutdown();
  free(g_latencies);

  return 0;
}
