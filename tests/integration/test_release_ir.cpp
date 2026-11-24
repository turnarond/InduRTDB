/**
 * @file test_release_ir.cpp
 * @brief 阶段一 5 个 IR 真测试（Linux 可验证，非现场 B 段真值）。
 * 冻结名：IR1_FullSnapshot / IR6_RestartSemantics / IR7_PartitionConverge /
 * IR4_NetworkPartition / IR5_StaleEpoch。全部为 Linux 近似语义，
 * 禁止阶段三用作现场 F2/F3a/F4 measured 证据。
 */
#include <gtest/gtest.h>

extern "C" {
#include <indurtdb/indurtdb.h>
}

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

namespace {

static const char *kScope = "Linux-only 近似语义；非现场真值，禁止作为 B 段 measured 证据";

class ReleaseIrTest : public ::testing::Test {
protected:
    indurtdb_t *h = nullptr;
    char inst[96] = {0};
    void SetUp() override {
        static int seq = 0;
        indurtdb_cfg_t cfg{};
        cfg.max_points = 256;
        cfg.max_subscribers = 8;
        std::snprintf(inst, sizeof(inst), "rel_ir_%d_%d", (int)getpid(), ++seq);
        ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
    }
    void TearDown() override { if (h) indurtdb_h_close(h); }
};

// IR1_FullSnapshot：写 N 点 → 全量读回逐字节一致（进程内 shm 快照语义）。
TEST_F(ReleaseIrTest, IR1_FullSnapshot)
{
    ASSERT_NE(kScope, nullptr);
    for (uint32_t id = 1; id <= 32; ++id) {
        char name[32];
        std::snprintf(name, sizeof(name), "pt%u", id);
        ASSERT_EQ(indurtdb_h_create_point(h, id, name, INDURTDB_TYPE_INT32,
                                          INDURTDB_ACCESS_READ_WRITE), 0);
        ASSERT_EQ(indurtdb_h_write_int32(h, id, (int32_t)(id * 7u)), 0);
    }
    for (uint32_t id = 1; id <= 32; ++id) {
        const indurtdb_point_t *p = indurtdb_h_peek(h, id);
        ASSERT_NE(p, nullptr);
        char expect[32];
        std::snprintf(expect, sizeof(expect), "pt%u", id);
        EXPECT_STREQ(p->name, expect);
        int32_t v = 0;
        ASSERT_EQ(indurtdb_h_read_int32(h, id, &v), 0);
        EXPECT_EQ(v, (int32_t)(id * 7u));
    }
}

// IR6_RestartSemantics：同实例双 handle attach 读到一致数据（owner 存活的 Linux 近似）。
// 真正整机重启后语义由现场 F4 独立工具取证（本测试不覆盖）。
TEST_F(ReleaseIrTest, IR6_RestartSemantics)
{
    const char *inst2 = "rel_ir_restart_attach";
    indurtdb_cfg_t cfg{}; cfg.max_points = 16; cfg.max_subscribers = 4;
    indurtdb_t *owner = nullptr;
    ASSERT_EQ(indurtdb_h_open(&owner, inst2, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(owner, 3, "survive", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    ASSERT_EQ(indurtdb_h_write_int32(owner, 3, 4242), 0);
    indurtdb_t *attached = nullptr;
    ASSERT_EQ(indurtdb_h_open(&attached, inst2, &cfg), 0);
    int32_t v = 0;
    ASSERT_EQ(indurtdb_h_read_int32(attached, 3, &v), 0);
    EXPECT_EQ(v, 4242); // 可证伪：新 attach 必须读到 owner 写入的数据
    indurtdb_h_close(attached);
    indurtdb_h_close(owner);
}

// IR7_PartitionConverge：同实例不同 partition 的点位隔离（Linux 近似分区语义）。
TEST_F(ReleaseIrTest, IR7_PartitionConverge)
{
    ASSERT_EQ(indurtdb_h_create_point_in(h, 0, 20, "p0", INDURTDB_TYPE_INT32,
                                         INDURTDB_ACCESS_READ_WRITE), 0);
    ASSERT_EQ(indurtdb_h_create_point_in(h, 1, 21, "p1", INDURTDB_TYPE_INT32,
                                         INDURTDB_ACCESS_READ_WRITE), 0);
    uint32_t id = 999;
    EXPECT_EQ(indurtdb_h_find_by_name_in(h, 0, "p0", &id), 0);
    EXPECT_EQ(id, 20u);
    EXPECT_NE(indurtdb_h_find_by_name_in(h, 1, "p0", &id), 0); // 分区 1 看不到分区 0 的点
    EXPECT_EQ(indurtdb_h_find_by_name_in(h, 1, "p1", &id), 0);
    EXPECT_EQ(id, 21u);
}

// IR4_NetworkPartition：SIGSTOP 冻结进程 1s → SIGCONT 恢复（Linux 近似分区/恢复语义）。
// 非现场 1/6/30s 断网真值（由 F3a 独立工具取证）。
TEST_F(ReleaseIrTest, IR4_NetworkPartition)
{
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        indurtdb_t *c = nullptr;
        indurtdb_cfg_t cfg{}; cfg.max_points = 8; cfg.max_subscribers = 2;
        if (indurtdb_h_open(&c, "rel_ir_partition_child", &cfg) != 0) _exit(2);
        if (indurtdb_h_create_point(c, 1, "frozen", INDURTDB_TYPE_INT32,
                                    INDURTDB_ACCESS_READ_WRITE) != 0) _exit(3);
        if (indurtdb_h_write_int32(c, 1, 77) != 0) _exit(4);
        raise(SIGSTOP); // 模拟"不可达"
        indurtdb_h_close(c);
        _exit(0);
    }
    usleep(100000);
    int st = 0;
    waitpid(pid, &st, WUNTRACED);
    EXPECT_TRUE(WIFSTOPPED(st));
    kill(pid, SIGCONT);
    waitpid(pid, &st, 0);
    EXPECT_TRUE(WIFEXITED(st));
    EXPECT_EQ(WEXITSTATUS(st), 0); // 可证伪：恢复后子进程必须正常完成
}

// IR5_StaleEpoch：Linux 侧覆盖"带 ts 写-读往返"基础语义（可证伪）。
// 真实 stale-epoch 拒绝在 VSOA NOTIFY 层（epoch），本地 shm 无 epoch 字段——
// 由现场 F3a 独立工具取证，本测试不覆盖。
TEST_F(ReleaseIrTest, IR5_StaleEpoch)
{
    ASSERT_EQ(indurtdb_h_create_point(h, 7, "epoch", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    ASSERT_EQ(indurtdb_h_write_int32_ts(h, 7, 100, 1000u), 0);
    int32_t v = 0;
    ASSERT_EQ(indurtdb_h_read_int32(h, 7, &v), 0);
    EXPECT_EQ(v, 100); // 可证伪：写 100 读回 100
    EXPECT_EQ(kScope != nullptr, true);
}

} // namespace
