/**
 * @file test_c_migration.cpp
 * @brief v1 -> 当前版(v3) 段迁移工具集成测试（红→绿）
 *
 * 测试策略：
 *  1. 进程内用原始字节造一个 v1 段（64B 旧头 + N×128B 点 + M×16B 心跳）；
 *  2. 通过 system() 调用 Python 迁移工具 tools/irt_migrate 执行 v1->v3 迁移；
 *  3. 用**真实的 InduRTDB 库 indurtdb_h_open** 回挂迁移产物，逐项校验
 *     （点位值/类型/质量/时间戳/名、source_ts 清零、find_by_name 索引）；
 *     库能成功 attach 即证明 Header 版本=IRT_SHM_VERSION 且 CRC 合法、字节级兼容。
 *  4. 双版本共存：v3.9 库必须拒绝挂载 v1 段（版本协商），证明新旧不混跑。
 */
#include <indurtdb/indurtdb.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

#ifndef INDURTDB_TOOLS_DIR
#define INDURTDB_TOOLS_DIR ""
#endif

static const uint32_t kMagic = 0x1DBA1DBAu;

/* v1 旧 Header（64B，与 src/internal/irt_types.h T1 之前一致） */
struct v1_header {
    uint32_t magic;
    uint32_t version;          /* = 1 */
    uint32_t max_points;
    uint32_t max_subscribers;
    uint64_t write_seq;
    int32_t  owner_pid;
    uint64_t writes;
    uint64_t timeouts;
    uint8_t  reserved[64 - 44];
} __attribute__((packed));

static std::string shm_name(const std::string& id) {
    return "/indurtdb_" + id;
}

static void write_v1_segment(const std::string& id, uint32_t N, uint32_t M,
                             const indurtdb_point_t* pts) {
    std::string path = "/dev/shm" + shm_name(id);
    ::unlink(path.c_str());
    int fd = ::shm_open(shm_name(id).c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    size_t total = sizeof(v1_header) + (size_t)N * sizeof(indurtdb_point_t)
                  + (size_t)M * 16u;
    ASSERT_EQ(::ftruncate(fd, (off_t)total), 0);
    void* m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, 0);
    ASSERT_NE(m, MAP_FAILED);
    v1_header h;
    std::memset(&h, 0, sizeof(h));
    h.magic = kMagic;
    h.version = 1;            /* v1 */
    h.max_points = N;
    h.max_subscribers = M;
    std::memcpy(m, &h, sizeof(v1_header));
    std::memcpy((char*)m + sizeof(v1_header), pts,
                (size_t)N * sizeof(indurtdb_point_t));
    ::munmap(m, total);
    ::close(fd);
}

static void remove_segment(const std::string& id) {
    std::string path = "/dev/shm" + shm_name(id);
    ::unlink(path.c_str());
}

/* 造一批带名/带 source_ts 的点位 */
static void fill_points(indurtdb_point_t* pts, uint32_t N) {
    for (uint32_t i = 0; i < N; ++i) {
        indurtdb_point_t* p = &pts[i];
        std::memset(p, 0, sizeof(*p));
        p->type = INDURTDB_TYPE_INT32;
        p->quality = INDURTDB_QUALITY_GOOD;
        p->value.i = (int32_t)(i * 7);
        p->timestamp_ns = (uint64_t)i * 1000u;
        /* 前 8 个点带名，用于索引迁移验证 */
        if (i < 8) {
            std::snprintf(p->name, sizeof(p->name), "tag_%03u", i);
        }
        /* source_ts 非 0，验证迁移默认清零 */
        p->source_timestamp_ns = (uint64_t)i * 11u;
    }
}

TEST(Migration, V1ToV3RoundTrip) {
    const uint32_t N = 16, M = 4;
    const std::string src = "mig_src";
    const std::string dst = "mig_dst";

    /* 清理残留 */
    remove_segment(src);
    remove_segment(dst);

    indurtdb_point_t pts[16];
    fill_points(pts, N);
    write_v1_segment(src, N, M, pts);

    /* 调用 Python 迁移工具（cd 到包目录以便 `python3 -m irt_migrate`） */
    std::string cmd = std::string("cd \"") + INDURTDB_TOOLS_DIR
                      + "/irt_migrate\" && python3 -m irt_migrate migrate"
                        " --src " + src + " --dst " + dst + " --force";
    int rc = ::system(cmd.c_str());
    ASSERT_EQ(WEXITSTATUS(rc), 0) << "迁移工具执行失败: " << cmd;

    /* 用真实库回挂迁移后的段（当前版 v3） */
    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    int open_rc = indurtdb_h_open(&h, dst.c_str(), &cfg);
    ASSERT_EQ(open_rc, 0) << "v3.9 库无法挂载迁移后的段"
                             "（版本/CRC/布局不兼容）";

    /* 逐项校验点位 */
    for (uint32_t i = 0; i < N; ++i) {
        indurtdb_point_t got;
        ASSERT_EQ(indurtdb_h_read_point(h, i, &got), 0);
        EXPECT_EQ(got.type, pts[i].type) << "point " << i << " type";
        EXPECT_EQ(got.quality, pts[i].quality) << "point " << i << " quality";
        EXPECT_EQ(got.timestamp_ns, pts[i].timestamp_ns)
            << "point " << i << " timestamp";
        EXPECT_EQ(std::memcmp(&got.value, &pts[i].value, sizeof(got.value)), 0)
            << "point " << i << " value";
        EXPECT_EQ(std::strncmp(got.name, pts[i].name, sizeof(got.name)), 0)
            << "point " << i << " name";
        /* source_ts 默认清零 */
        EXPECT_EQ(got.source_timestamp_ns, 0u)
            << "point " << i << " source_ts 应被清零";
    }

    /* 索引迁移：按名定位 */
    uint32_t id = 0;
    EXPECT_EQ(indurtdb_h_find_by_name(h, "tag_002", &id), 0);
    EXPECT_EQ(id, 2u);
    /* 无名点不应命中 */
    uint32_t missing = 0;
    EXPECT_NE(indurtdb_h_find_by_name(h, "no_such_tag", &missing), 0);

    indurtdb_h_close(h);
    remove_segment(dst);
    remove_segment(src);
}

TEST(Migration, V34RejectsV1Segment) {
    /* 双版本共存：v3.9 库必须拒绝挂载 v1 段（版本协商拒绝） */
    const uint32_t N = 8, M = 0;
    const std::string id = "mig_v1only";
    remove_segment(id);

    indurtdb_point_t pts[8];
    fill_points(pts, N);
    write_v1_segment(id, N, M, pts);

    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    int rc = indurtdb_h_open(&h, id.c_str(), &cfg);
    EXPECT_NE(rc, 0) << "v3.4 库不应挂载 v1 段（应拒绝）";

    remove_segment(id);
}

/* ============================================================================
 * T8：v2 -> v3 迁移覆盖（layout v2 源段，128B Header）
 * ==========================================================================*/

/* 标准 CRC-32/IEEE（zlib.crc32 语义）：反射多项式 0xEDB88320，
 * 初值 0xFFFFFFFF，末尾异或 0xFFFFFFFF。自包含实现，不引入新依赖。 */
static uint32_t crc32_step(uint32_t crc, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        for (int k = 0; k < 8; ++k) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

static void put_u32(uint8_t* p, uint32_t v) {
    std::memcpy(p, &v, sizeof(v));
}

static void put_u64(uint8_t* p, uint64_t v) {
    std::memcpy(p, &v, sizeof(v));
}

/* roundup_pow2(max(8, N*2)) —— 与 irt_layout_buckets 一致 */
static uint32_t layout_buckets(uint32_t max_points) {
    uint32_t cap = 8u;
    while (cap < max_points * 2u) {
        cap <<= 1u;
    }
    return cap;
}

/* 造一个 128B Header 的段（v2/v3 逐字节同构，仅 version 不同）。
 * version==2 时为合法 v2 源段；version 传其它值用于"拒绝错误源版本"用例。 */
static void write_segment_versioned(const std::string& id, uint32_t N, uint32_t M,
                                    const indurtdb_point_t* pts,
                                    const indurtdb_meta_t* meta,
                                    uint32_t version) {
    const uint32_t off_points = 128u;
    const uint32_t off_index  = off_points + N * 128u;
    const uint32_t buckets    = layout_buckets(N);
    const uint32_t off_meta   = off_index + buckets * 8u;
    const uint32_t off_subs   = off_meta + N * 32u;
    const size_t   total      = (size_t)off_subs + (size_t)M * 16u;

    std::string path = "/dev/shm" + shm_name(id);
    ::unlink(path.c_str());
    int fd = ::shm_open(shm_name(id).c_str(), O_CREAT | O_RDWR, 0600);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::ftruncate(fd, (off_t)total), 0);
    void* m = ::mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ASSERT_NE(m, MAP_FAILED);
    std::memset(m, 0, total);

    uint8_t hdr[128];
    std::memset(hdr, 0, sizeof(hdr));
    put_u32(hdr + 0,  kMagic);
    put_u32(hdr + 4,  version);
    put_u32(hdr + 8,  N);
    put_u32(hdr + 12, M);
    put_u32(hdr + 56, off_points);
    put_u32(hdr + 60, off_index);
    put_u32(hdr + 64, off_meta);
    put_u32(hdr + 68, off_subs);
    /* CRC 覆盖 [0:16) 与 [48:72)，存储于 offset 44 */
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_step(crc, hdr, 16u);
    crc = crc32_step(crc, hdr + 48, 24u);
    put_u32(hdr + 44, crc ^ 0xFFFFFFFFu);

    std::memcpy(m, hdr, sizeof(hdr));
    std::memcpy((char*)m + off_points, pts,
                (size_t)N * sizeof(indurtdb_point_t));
    /* v2 源段索引内容无意义（工具会重建），全 0xFF => 全部 EMPTY，肉眼可见无效 */
    std::memset((char*)m + off_index, 0xFF, (size_t)buckets * 8u);
    std::memcpy((char*)m + off_meta, meta,
                (size_t)N * sizeof(indurtdb_meta_t));
    /* subs 区保持全 0 */
    ::munmap(m, total);
    ::close(fd);
}

static void write_v2_segment(const std::string& id, uint32_t N, uint32_t M,
                             const indurtdb_point_t* pts,
                             const indurtdb_meta_t* meta) {
    write_segment_versioned(id, N, M, pts, meta, 2u);
}

/* v2 点位：值/类型/质量/时间戳/名 + 故意为垃圾的 partition(offset 120) */
static void fill_v2_points(indurtdb_point_t* pts, uint32_t N) {
    for (uint32_t i = 0; i < N; ++i) {
        indurtdb_point_t* p = &pts[i];
        std::memset(p, 0, sizeof(*p));
        p->type = INDURTDB_TYPE_INT32;
        p->quality = INDURTDB_QUALITY_GOOD;
        p->value.i = (int32_t)(1000 + i * 7u);
        p->timestamp_ns = (uint64_t)i * 1000u;
        if (i < 8) {
            std::snprintf(p->name, sizeof(p->name), "v2_tag_%03u", i);
        }
        p->source_timestamp_ns = 0x11110000u + i;
        /* v2 该 4B 是 reserved，迁移必须强制清零 */
        p->partition = 0xDEAD0000u + i;
    }
}

/* v2 元数据：可识别的非零值，迁移须逐字节保留 */
static void fill_v2_meta(indurtdb_meta_t* meta, uint32_t N) {
    for (uint32_t i = 0; i < N; ++i) {
        std::memset(&meta[i], 0, sizeof(meta[i]));
        meta[i].eur_min  = 10.0 + (double)i;
        meta[i].eur_max  = 20.0 + (double)i;
        meta[i].deadband = 0.5f;
        meta[i].flags    = INDURTDB_META_FLAG_EUR;
    }
}

/* 返回 system() 的原始状态，便于调用方同时判断异常退出与退出码 */
static int run_migrate(const std::string& src, const std::string& dst) {
    std::string cmd = std::string("cd \"") + INDURTDB_TOOLS_DIR
                      + "/irt_migrate\" && python3 -m irt_migrate migrate"
                        " --src " + src + " --dst " + dst + " --force";
    return ::system(cmd.c_str());
}

TEST(Migrate, V2ToV3PreservesPoints) {
    const uint32_t N = 16, M = 2;
    const std::string src = "mig_v2_src";
    const std::string dst = "mig_v2_dst";
    remove_segment(src);
    remove_segment(dst);

    indurtdb_point_t pts[16];
    indurtdb_meta_t  meta[16];
    fill_v2_points(pts, N);
    fill_v2_meta(meta, N);
    write_v2_segment(src, N, M, pts, meta);

    int mrc = run_migrate(src, dst);
    ASSERT_TRUE(WIFEXITED(mrc)) << "迁移工具异常退出";
    ASSERT_EQ(WEXITSTATUS(mrc), 0) << "v2->v3 迁移失败";

    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, dst.c_str(), &cfg), 0)
        << "v3.9 库无法挂载 v2 迁移产物";

    for (uint32_t i = 0; i < N; ++i) {
        indurtdb_point_t got;
        ASSERT_EQ(indurtdb_h_read_point(h, i, &got), 0);
        EXPECT_EQ(got.type, pts[i].type) << "point " << i << " type";
        EXPECT_EQ(got.quality, pts[i].quality) << "point " << i << " quality";
        EXPECT_EQ(got.timestamp_ns, pts[i].timestamp_ns)
            << "point " << i << " timestamp";
        EXPECT_EQ(std::memcmp(&got.value, &pts[i].value, sizeof(got.value)), 0)
            << "point " << i << " value";
        EXPECT_EQ(std::strncmp(got.name, pts[i].name, sizeof(got.name)), 0)
            << "point " << i << " name";
        EXPECT_EQ(got.source_timestamp_ns, 0u)
            << "point " << i << " source_ts 应被清零";

        /* v2 元数据区必须原样保留 */
        indurtdb_meta_t m;
        ASSERT_EQ(indurtdb_h_get_meta(h, i, &m), 0);
        EXPECT_DOUBLE_EQ(m.eur_min, meta[i].eur_min) << "point " << i << " eur_min";
        EXPECT_DOUBLE_EQ(m.eur_max, meta[i].eur_max) << "point " << i << " eur_max";
        EXPECT_FLOAT_EQ(m.deadband, meta[i].deadband) << "point " << i << " deadband";
        EXPECT_EQ(m.flags, meta[i].flags) << "point " << i << " flags";
    }

    uint32_t id = 0;
    EXPECT_EQ(indurtdb_h_find_by_name(h, "v2_tag_002", &id), 0);
    EXPECT_EQ(id, 2u);

    indurtdb_h_close(h);
    remove_segment(dst);
    remove_segment(src);
}

TEST(Migrate, PartitionSetToZero) {
    const uint32_t N = 16, M = 0;
    const std::string src = "mig_part_src";
    const std::string dst = "mig_part_dst";
    remove_segment(src);
    remove_segment(dst);

    indurtdb_point_t pts[16];
    indurtdb_meta_t  meta[16];
    fill_v2_points(pts, N);
    fill_v2_meta(meta, N);
    write_v2_segment(src, N, M, pts, meta);

    int mrc = run_migrate(src, dst);
    ASSERT_TRUE(WIFEXITED(mrc)) << "迁移工具异常退出";
    ASSERT_EQ(WEXITSTATUS(mrc), 0) << "v2->v3 迁移失败";

    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, dst.c_str(), &cfg), 0);

    for (uint32_t i = 0; i < N; ++i) {
        indurtdb_point_t got;
        ASSERT_EQ(indurtdb_h_read_point(h, i, &got), 0);
        EXPECT_EQ(got.partition, 0u)
            << "point " << i << " partition 应被强制置 0";
    }

    uint32_t id = 0;
    EXPECT_EQ(indurtdb_h_find_by_name_in(h, 0, "v2_tag_001", &id), 0)
        << "默认分区 0 内应能按名命中";
    uint32_t other = 0;
    EXPECT_NE(indurtdb_h_find_by_name_in(h, 7, "v2_tag_001", &other), 0)
        << "非 0 分区不应命中";

    indurtdb_h_close(h);
    remove_segment(dst);
    remove_segment(src);
}

TEST(Migrate, ChecksumVerification) {
    const uint32_t N = 8, M = 1;
    const std::string src = "mig_crc_src";
    const std::string dst = "mig_crc_dst";
    remove_segment(src);
    remove_segment(dst);

    indurtdb_point_t pts[8];
    indurtdb_meta_t  meta[8];
    fill_v2_points(pts, N);
    fill_v2_meta(meta, N);
    write_v2_segment(src, N, M, pts, meta);

    int mrc = run_migrate(src, dst);
    ASSERT_TRUE(WIFEXITED(mrc)) << "迁移工具异常退出";
    ASSERT_EQ(WEXITSTATUS(mrc), 0) << "v2->v3 迁移失败";

    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    /* 库 attach 时校验 CRC + version，能挂上本身即 CRC 正确的证据 */
    ASSERT_EQ(indurtdb_h_open(&h, dst.c_str(), &cfg), 0);
    indurtdb_h_close(h);

    /* 篡改 offset 60（off_index，落在 CRC 覆盖的 [48:72) 内） */
    std::string path = "/dev/shm" + shm_name(dst);
    int fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    uint8_t orig = 0;
    ASSERT_EQ(::pread(fd, &orig, 1, (off_t)60), 1);
    uint8_t flipped = (uint8_t)(orig ^ 0x01u);
    ASSERT_EQ(::pwrite(fd, &flipped, 1, (off_t)60), 1);
    ASSERT_EQ(::close(fd), 0);

    indurtdb_t* h2 = nullptr;
    EXPECT_NE(indurtdb_h_open(&h2, dst.c_str(), &cfg), 0)
        << "CRC 被篡改后必须拒绝挂载";

    /* 还原字节，证明拒绝仅由 CRC 变化导致 */
    fd = ::open(path.c_str(), O_RDWR);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::pwrite(fd, &orig, 1, (off_t)60), 1);
    ASSERT_EQ(::close(fd), 0);

    indurtdb_t* h3 = nullptr;
    EXPECT_EQ(indurtdb_h_open(&h3, dst.c_str(), &cfg), 0)
        << "还原后应可再次挂载";
    if (h3) indurtdb_h_close(h3);

    remove_segment(dst);
    remove_segment(src);
}

TEST(Migrate, RefusesWrongSourceVersion) {
    const uint32_t N = 8, M = 0;
    indurtdb_point_t pts[8];
    indurtdb_meta_t  meta[8];
    fill_v2_points(pts, N);
    fill_v2_meta(meta, N);

    /* A：version=3（已是目标格式，不得作为迁移源） */
    {
        const std::string src = "mig_badver3";
        const std::string dst = "mig_badver3_dst";
        remove_segment(src);
        remove_segment(dst);
        write_segment_versioned(src, N, M, pts, meta, 3u);

        int rc = run_migrate(src, dst);
        EXPECT_TRUE(WIFEXITED(rc)) << "迁移工具异常退出";
        EXPECT_NE(WEXITSTATUS(rc), 0) << "version=3 源段必须被拒绝";

        remove_segment(dst);
        remove_segment(src);
    }

    /* B：version=7（未知版本） */
    {
        const std::string src = "mig_badver7";
        const std::string dst = "mig_badver7_dst";
        remove_segment(src);
        remove_segment(dst);
        write_segment_versioned(src, N, M, pts, meta, 7u);

        int rc = run_migrate(src, dst);
        EXPECT_TRUE(WIFEXITED(rc)) << "迁移工具异常退出";
        EXPECT_NE(WEXITSTATUS(rc), 0) << "version=7 源段必须被拒绝";

        remove_segment(dst);
        remove_segment(src);
    }
}
