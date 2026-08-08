/**
 * @file test_shared_memory_segment.cpp
 * @brief SharedMemorySegment 单元测试 —— 真实 POSIX 共享内存段
 * @version 2.1.0
 *
 * 使用真实 shm_open/mmap 验证创建者/使用者区分、Header 校验、
 * 布局偏移与跨段数据可见性。段名带 pid, 测试结束由 owner 析构 unlink。
 */

#include <indurtdb/core/shared_memory_segment.hpp>
#include <indurtdb/types/memory_layout.hpp>
#include <gtest/gtest.h>
#include <unistd.h>
#include <cstdio>
#include <string>

namespace {

using indurtdb::core::SharedMemorySegment;
using indurtdb::InduRTDBHeader;
using indurtdb::PointData;
using indurtdb::PointType;
using indurtdb::Quality;
using indurtdb::SubscriberEntry;

// 生成唯一段名: 真实实现映射为 /dev/shm/indurtdb_<name>
static std::string unique_name(const char* tag) {
    char buf[80];
    snprintf(buf, sizeof(buf), "tst_%s_%d", tag, (int)getpid());
    return buf;
}

// 创建者初始化 Header (magic/version/布局参数)
TEST(SharedMemorySegmentTest, OwnerInitializesHeader) {
    SharedMemorySegment seg(unique_name("owner").c_str(), 16, 4);
    ASSERT_TRUE(seg.initialize());
    EXPECT_TRUE(seg.is_owner());
    ASSERT_NE(seg.header(), nullptr);
    EXPECT_EQ(seg.header()->max_points, 16);
    EXPECT_EQ(seg.header()->max_subscribers, 4);
    EXPECT_EQ(seg.header()->write_seq, 0);
}

// 第二个实例 attach 到已存在段: 非 owner, Header 校验通过
TEST(SharedMemorySegmentTest, SecondAttachValidatesHeader) {
    std::string name = unique_name("attach");
    SharedMemorySegment seg1(name.c_str(), 16, 4);
    ASSERT_TRUE(seg1.initialize());

    SharedMemorySegment seg2(name.c_str(), 16, 4);
    ASSERT_TRUE(seg2.initialize());
    EXPECT_FALSE(seg2.is_owner());
    ASSERT_NE(seg2.header(), nullptr);
    EXPECT_EQ(seg2.header()->max_points, 16);
}

// magic 被破坏后, 新 attach 必须拒绝初始化
TEST(SharedMemorySegmentTest, MagicMismatchRejected) {
    std::string name = unique_name("badmagic");
    SharedMemorySegment seg1(name.c_str(), 16, 4);
    ASSERT_TRUE(seg1.initialize());

    auto* h = static_cast<InduRTDBHeader*>(seg1.base());
    uint32_t orig_magic = h->magic;
    h->magic = 0xDEADBEEF;

    SharedMemorySegment seg2(name.c_str(), 16, 4);
    EXPECT_FALSE(seg2.initialize());

    h->magic = orig_magic;  // 恢复, 保证 owner 析构清理正常
}

// 布局偏移: Header | PointData[] | SubscriberEntry[]
TEST(SharedMemorySegmentTest, LayoutOffsetsMatch) {
    SharedMemorySegment seg(unique_name("layout").c_str(), 16, 4);
    ASSERT_TRUE(seg.initialize());

    EXPECT_EQ(seg.total_size(),
              sizeof(InduRTDBHeader)
              + 16 * sizeof(PointData)
              + 4 * sizeof(SubscriberEntry));

    auto* base = static_cast<char*>(seg.base());
    EXPECT_EQ(seg.points(), reinterpret_cast<PointData*>(base + sizeof(InduRTDBHeader)));
    EXPECT_EQ(seg.subscribers(), reinterpret_cast<SubscriberEntry*>(
                 base + sizeof(InduRTDBHeader) + 16 * sizeof(PointData)));
}

// 跨段可见性: 段 A 写入点位, 段 B 立即读到 (真实共享内存验证)
TEST(SharedMemorySegmentTest, WriteVisibleAcrossSegments) {
    std::string name = unique_name("share");
    SharedMemorySegment seg1(name.c_str(), 16, 4);
    SharedMemorySegment seg2(name.c_str(), 16, 4);
    ASSERT_TRUE(seg1.initialize());
    ASSERT_TRUE(seg2.initialize());

    PointData* p1 = seg1.points() + 3;
    p1->value.d = 77.7;
    p1->type = PointType::DOUBLE;
    p1->quality = Quality::GOOD;
    p1->timestamp_ns = 5;

    const PointData* p2 = seg2.points() + 3;
    EXPECT_DOUBLE_EQ(p2->value.d, 77.7);
    EXPECT_EQ(p2->type, PointType::DOUBLE);
    EXPECT_EQ(p2->quality, Quality::GOOD);
    EXPECT_EQ(p2->timestamp_ns, 5);
}

// shutdown 后可重新 initialize 复用对象
TEST(SharedMemorySegmentTest, ShutdownThenReinitialize) {
    SharedMemorySegment seg(unique_name("restart").c_str(), 16, 4);
    ASSERT_TRUE(seg.initialize());
    seg.shutdown();
    EXPECT_EQ(seg.base(), nullptr);
    ASSERT_TRUE(seg.initialize());
    EXPECT_EQ(seg.header()->max_points, 16);
}

} // namespace
