/**
 * @file test_c_delta.cpp
 * @brief 运行时变更 delta 日志单元测试（v3.7 主题B B4）
 *
 * 覆盖 irt_delta 纯解析行为（不依赖共享内存）：
 *   - 记录布局 80B 静态断言
 *   - 空文件 / 文件不存在 → 0
 *   - 正常序列按序回放
 *   - 末尾残片（不足 80B）被丢弃，其前完整记录仍生效
 *   - 坏 magic / 坏 version / 未知 op → 停止，已应用部分保留
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include "core/irt_delta.h"
}

namespace {

struct Rec {
    uint16_t op;
    uint32_t id;
    uint8_t  type;
    uint8_t  access;
    std::string name;
};

std::vector<Rec> g_applied;

int apply_fn(const irt_delta_rec_t* rec, void* ctx)
{
    (void)ctx;
    Rec r;
    r.op     = rec->op;
    r.id     = rec->point_id;
    r.type   = rec->type;
    r.access = rec->access;
    r.name   = std::string(rec->name);
    g_applied.push_back(r);
    return 0;
}

std::string tmp_path(const char* tag)
{
    return std::string("/tmp/irt_delta_") + tag + ".bin";
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& data)
{
    unlink(path.c_str());
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_NE(fd, -1);
    if (!data.empty()) {
        ASSERT_EQ(write(fd, data.data(), data.size()), (ssize_t)data.size());
    }
    close(fd);
}

std::vector<uint8_t> rec_bytes(uint16_t op, uint32_t id, const char* name)
{
    irt_delta_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic    = IRT_DELTA_MAGIC;
    r.version  = IRT_DELTA_VERSION;
    r.op       = op;
    r.point_id = id;
    r.type     = 1;
    r.access   = 3;
    if (name) strncpy(r.name, name, sizeof(r.name) - 1);
    std::vector<uint8_t> v(sizeof(r));
    memcpy(v.data(), &r, sizeof(r));
    return v;
}

void prepend(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src)
{
    dst.insert(dst.end(), src.begin(), src.end());
}

} // namespace

/* 1. 记录布局锁死 80B */
TEST(Delta, RecordLayoutIs80Bytes)
{
    EXPECT_EQ(sizeof(irt_delta_rec_t), 80u);
}

/* 2. 文件不存在 / 空文件 → 0 条 */
TEST(Delta, MissingOrEmptyFileYieldsZero)
{
    g_applied.clear();
    EXPECT_EQ(irt_delta_replay("/tmp/irt_delta_definitely_missing.bin", apply_fn, nullptr), 0);

    std::string p = tmp_path("empty");
    write_bytes(p, {});
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 0);
    EXPECT_TRUE(g_applied.empty());
    unlink(p.c_str());
}

/* 3. 正常序列按序回放 */
TEST(Delta, ReplaysSequenceInOrder)
{
    std::string p = tmp_path("seq");
    std::vector<uint8_t> data;
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 10, "P10"));
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 11, "P11"));
    prepend(data, rec_bytes(IRT_DELTA_OP_RENAME, 11, "P11_new"));
    prepend(data, rec_bytes(IRT_DELTA_OP_DELETE, 10, nullptr));
    write_bytes(p, data);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 4);
    ASSERT_EQ(g_applied.size(), 4u);
    EXPECT_EQ(g_applied[0].op, IRT_DELTA_OP_CREATE);
    EXPECT_EQ(g_applied[0].id, 10u);
    EXPECT_EQ(g_applied[0].name, "P10");
    EXPECT_EQ(g_applied[2].op, IRT_DELTA_OP_RENAME);
    EXPECT_EQ(g_applied[2].name, "P11_new");
    EXPECT_EQ(g_applied[3].op, IRT_DELTA_OP_DELETE);

    unlink(p.c_str());
}

/* 4. 末尾残片被丢弃，其前完整记录仍生效 */
TEST(Delta, TrailingPartialRecordIsDiscarded)
{
    std::string p = tmp_path("frag");
    std::vector<uint8_t> data;
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 10, "P10"));
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 11, "P11"));
    /* 追加 30B 残片（不足一条 80B） */
    std::vector<uint8_t> full = rec_bytes(IRT_DELTA_OP_CREATE, 12, "P12");
    data.insert(data.end(), full.begin(), full.begin() + 30);
    write_bytes(p, data);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 2);
    ASSERT_EQ(g_applied.size(), 2u);
    EXPECT_EQ(g_applied[1].id, 11u);   /* 残片中的 12 不生效 */

    unlink(p.c_str());
}

/* 5. 坏 magic 停止回放，已应用部分保留 */
TEST(Delta, BadMagicStopsReplay)
{
    std::string p = tmp_path("badmagic");
    std::vector<uint8_t> data;
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 10, "P10"));
    std::vector<uint8_t> bad = rec_bytes(IRT_DELTA_OP_CREATE, 11, "P11");
    uint32_t junk = 0xDEADBEEFu;
    memcpy(bad.data(), &junk, sizeof(junk));   /* 覆盖 magic */
    prepend(data, bad);
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 12, "P12"));
    write_bytes(p, data);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 1);
    ASSERT_EQ(g_applied.size(), 1u);
    EXPECT_EQ(g_applied[0].id, 10u);

    unlink(p.c_str());
}

/* 6. 坏 version 停止回放 */
TEST(Delta, BadVersionStopsReplay)
{
    std::string p = tmp_path("badver");
    std::vector<uint8_t> data;
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 10, "P10"));
    std::vector<uint8_t> bad = rec_bytes(IRT_DELTA_OP_CREATE, 11, "P11");
    uint16_t v = 9999;
    memcpy(bad.data() + 4, &v, sizeof(v));   /* 覆盖 version */
    prepend(data, bad);
    write_bytes(p, data);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 1);
    unlink(p.c_str());
}

/* 7. 未知 op 停止回放 */
TEST(Delta, UnknownOpStopsReplay)
{
    std::string p = tmp_path("badop");
    std::vector<uint8_t> data;
    prepend(data, rec_bytes(IRT_DELTA_OP_CREATE, 10, "P10"));
    prepend(data, rec_bytes(99, 11, "P11"));   /* 非法 op */
    write_bytes(p, data);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 1);
    unlink(p.c_str());
}

/* 8. append + 回放往返（经真实文件） */
TEST(Delta, AppendThenReplayRoundTrip)
{
    std::string p = tmp_path("roundtrip");
    unlink(p.c_str());

    int fd = irt_delta_open_append(p.c_str());
    ASSERT_GE(fd, 0);

    irt_delta_rec_t r;
    memset(&r, 0, sizeof(r));
    r.magic    = IRT_DELTA_MAGIC;
    r.version  = IRT_DELTA_VERSION;
    r.op       = IRT_DELTA_OP_CREATE;
    r.point_id = 42;
    r.type     = 1;
    r.access   = 3;
    strncpy(r.name, "RT42", sizeof(r.name) - 1);
    ASSERT_EQ(irt_delta_append(fd, &r), 0);
    close(fd);

    g_applied.clear();
    EXPECT_EQ(irt_delta_replay(p.c_str(), apply_fn, nullptr), 1);
    ASSERT_EQ(g_applied.size(), 1u);
    EXPECT_EQ(g_applied[0].id, 42u);
    EXPECT_EQ(g_applied[0].name, "RT42");

    unlink(p.c_str());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
