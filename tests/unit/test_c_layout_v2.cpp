/**
 * @file test_c_layout_v2.cpp
 * @brief v3.4 T1 —— 段布局 v2：Header 128B、Header CRC、区段偏移、版本协商
 *
 * 红阶段：以下断言针对尚未存在的布局 v2 能力，当前应失败。
 *   - sizeof(irt_header_t) == 128
 *   - 点位区起始偏移 == 128（由 Header 区段偏移给出，不再硬算 sizeof）
 *   - Header 可 seal（写 CRC）并 verify；篡改后 verify 失败
 *   - 挂载 version=1 的旧段被**明确拒绝**（返回布局不兼容错误码，而非 -1 通用失败）
 */
#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

extern "C" {
#include <indurtdb/indurtdb.h>

#include "core/irt_shm.h"
#include "internal/irt_types.h"
}

namespace {

/* v1 头部布局（64B），用于构造"旧段"以验证拒绝挂载 */
struct V1Header {
    uint32_t magic;
    uint32_t version;
    uint32_t max_points;
    uint32_t max_subscribers;
    uint64_t write_seq;
    int32_t  owner_pid;
    struct { uint64_t writes; uint64_t timeouts; } stats;
};

bool create_v1_segment(const char* instance, uint32_t max_points, uint32_t max_subs)
{
    std::string name = std::string(IRT_SHM_PREFIX) + instance;
    size_t size = 64u + (size_t)max_points * 128u + (size_t)max_subs * 16u;

    shm_unlink(name.c_str());
    int fd = shm_open(name.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)size) != 0) { close(fd); return false; }

    void* base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return false; }

    V1Header* h = (V1Header*)base;
    memset(h, 0, sizeof(*h));
    h->magic           = IRT_MAGIC;
    h->version         = 1u;            /* 旧布局版本 */
    h->max_points      = max_points;
    h->max_subscribers = max_subs;
    h->owner_pid       = (int32_t)getpid();

    munmap(base, size);
    close(fd);
    return true;
}

} // namespace

/* 1. Header v2 为 128 字节 */
TEST(LayoutV2, HeaderSizeIs128)
{
    EXPECT_EQ(sizeof(irt_header_t), 128u) << "v3.4 布局 v2: Header 扩至 128B";
}

/* 2. 点位区起始偏移由 Header 区段偏移给出，且为 128 */
TEST(LayoutV2, PointSectionStartsAt128)
{
    irt_shm_t s;
    ASSERT_EQ(irt_shm_init(&s, "layout_v2_off", 32, 4), 0);

    irt_header_t* hdr = irt_shm_header(&s);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->off_points, 128u);

    char* base  = (char*)hdr;
    char* pts   = (char*)irt_shm_points(&s);
    EXPECT_EQ((size_t)(pts - base), 128u) << "点位区不得再按 sizeof(header) 硬算";

    irt_shm_shutdown(&s);
}

/* 3. seal 后 verify 通过 */
TEST(LayoutV2, SealThenVerifyOk)
{
    irt_shm_t s;
    ASSERT_EQ(irt_shm_init(&s, "layout_v2_seal", 16, 2), 0);

    irt_header_t* hdr = irt_shm_header(&s);
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(irt_header_seal(hdr), 0);
    EXPECT_EQ(irt_header_verify(hdr), 1) << "seal 之后必须校验通过";

    irt_shm_shutdown(&s);
}

/* 4. 篡改 Header 任一字节后 CRC 校验失败 */
TEST(LayoutV2, CrcDetectsCorruption)
{
    irt_shm_t s;
    ASSERT_EQ(irt_shm_init(&s, "layout_v2_crc", 16, 2), 0);

    irt_header_t* hdr = irt_shm_header(&s);
    ASSERT_NE(hdr, nullptr);
    ASSERT_EQ(irt_header_seal(hdr), 0);

    uint32_t saved = hdr->max_points;
    hdr->max_points = saved ^ 0x1u;   /* 篡改 */
    EXPECT_EQ(irt_header_verify(hdr), 0) << "Header 被篡改必须能被 CRC 检出";
    hdr->max_points = saved;
    EXPECT_EQ(irt_header_verify(hdr), 1);

    irt_shm_shutdown(&s);
}

/* 5. 挂载 v1 旧段被明确拒绝（布局不兼容错误码，不是通用失败） */
TEST(LayoutV2, RejectsV1Segment)
{
    ASSERT_TRUE(create_v1_segment("layout_v2_v1seg", 16, 2));

    irt_shm_t s;
    int rc = irt_shm_init(&s, "layout_v2_v1seg", 16, 2);
    EXPECT_EQ(rc, IRT_SHM_ERR_VERSION)
        << "v1 段必须被拒绝挂载，且返回独立的版本/布局不兼容错误码";
    if (rc == 0) irt_shm_shutdown(&s);

    shm_unlink((std::string(IRT_SHM_PREFIX) + "layout_v2_v1seg").c_str());
}
