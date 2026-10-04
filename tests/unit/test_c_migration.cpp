/**
 * @file test_c_migration.cpp
 * @brief T5: v1 -> v2 段迁移工具集成测试（红→绿）
 *
 * 测试策略：
 *  1. 进程内用原始字节造一个 v1 段（64B 旧头 + N×128B 点 + M×16B 心跳）；
 *  2. 通过 system() 调用 Python 迁移工具 tools/irt_migrate 执行 v1->v2 迁移；
 *  3. 用**真实的 InduRTDB 库 indurtdb_h_open** 回挂 v2 段，逐项校验
 *     （点位值/类型/质量/时间戳/名、source_ts 清零、find_by_name 索引）；
 *     库能成功 attach 即证明 v2 Header 版本=2 且 CRC 合法、字节级兼容。
 *  4. 双版本共存：v3.4 库必须拒绝挂载 v1 段（版本协商），证明新旧不混跑。
 */
#include <indurtdb/indurtdb.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

TEST(Migration, V1ToV2RoundTrip) {
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

    /* 用真实库回挂 v2 段 */
    indurtdb_cfg_t cfg = {N, M};
    indurtdb_t* h = nullptr;
    int open_rc = indurtdb_h_open(&h, dst.c_str(), &cfg);
    ASSERT_EQ(open_rc, 0) << "v3.4 库无法挂载迁移后的 v2 段"
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
    /* 双版本共存：v3.4 库必须拒绝挂载 v1 段（版本协商拒绝） */
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
