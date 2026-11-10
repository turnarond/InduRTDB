/**
 * @file test_vsoa_cluster.cpp
 * @brief v3.12 M2：6 节点编排 + 故障注入 harness 的**基础设施**验证。
 *
 * 覆盖范围（明确声明，勿夸大）：本用例只验证**编排与故障注入地基**：
 *   1. 起 6 个节点（各独立 VSOA 服务 + 独立端口），逐节点 ping 通；
 *   2. 注入崩溃（SIGKILL）→ 该节点探测失败，**其余节点不受影响**；
 *   3. 注入暂停（SIGSTOP，模拟断链/分区）→ 该节点探测失败，恢复（SIGCONT）后重新可服务；
 *   4. 崩溃节点重启 → 端口变化且重新可服务。
 *
 * ⚠ 本用例**不验证集群同步语义**（增量/全量同步、epoch 隔离、分区收敛）——
 * 那是 IR1–IR7 的实现期工作，以本 harness 为载体后续补齐。
 * ⚠ 重启后**订阅重建**（§8.3-5）当前未实现（M1 留白），故只验证「恢复服务能力」。
 */
#include <gtest/gtest.h>

#include "vsoa_cluster_harness.h"

#include <chrono>
#include <thread>

using vsoa_cluster::Fault;
using vsoa_cluster::Harness;

TEST(VsoaCluster, SixNodesStartAndAreReachable)
{
    Harness h;
    ASSERT_TRUE(h.start(6)) << h.last_error();
    ASSERT_EQ(h.size(), 6u);
    /* 各节点端口必须互不相同（否则拓扑是假的） */
    for (size_t i = 0; i < h.size(); ++i) {
        EXPECT_NE(h.node(static_cast<int>(i)).port, 0u);
        for (size_t j = i + 1; j < h.size(); ++j) {
            EXPECT_NE(h.node(static_cast<int>(i)).port, h.node(static_cast<int>(j)).port);
        }
    }
    for (int i = 0; i < 6; ++i) {
        EXPECT_TRUE(h.probe(i)) << "node " << i << " unreachable: " << h.last_error();
    }
}

TEST(VsoaCluster, CrashOneNodeDoesNotAffectOthers)
{
    Harness h;
    ASSERT_TRUE(h.start(6)) << h.last_error();
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(h.probe(i)) << "node " << i << " unreachable before fault";
    }

    ASSERT_TRUE(h.inject(2, Fault::Crash)) << h.last_error();
    /* 故障节点探测必须失败（故障注入生效的判据） */
    EXPECT_FALSE(h.probe(2, 800));
    /* 其余节点不受影响 */
    for (int i = 0; i < 6; ++i) {
        if (i == 2) {
            continue;
        }
        EXPECT_TRUE(h.probe(i)) << "node " << i << " affected by node2 crash";
    }
}

TEST(VsoaCluster, PauseNodeSimulatesLinkLossAndRecovers)
{
    Harness h;
    ASSERT_TRUE(h.start(6)) << h.last_error();
    ASSERT_TRUE(h.probe(1));

    /* 暂停 = 进程在但不通（模拟断链 / 网络分区，§8.3-7） */
    ASSERT_TRUE(h.inject(1, Fault::Pause)) << h.last_error();
    EXPECT_FALSE(h.probe(1, 800));
    /* 其余节点仍可服务 */
    EXPECT_TRUE(h.probe(0));
    EXPECT_TRUE(h.probe(2));

    /* 恢复：SIGCONT 后重新可服务 */
    ASSERT_TRUE(h.clear(1)) << h.last_error();
    for (int i = 0; i < 40 && !h.probe(1, 300); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(h.probe(1)) << "node1 did not recover after SIGCONT";
}

TEST(VsoaCluster, RestartedNodeServesAgainOnNewPort)
{
    Harness h;
    ASSERT_TRUE(h.start(6)) << h.last_error();
    const uint16_t old_port = h.node(3).port;
    ASSERT_TRUE(h.probe(3));

    ASSERT_TRUE(h.inject(3, Fault::Crash)) << h.last_error();
    EXPECT_FALSE(h.probe(3, 800));

    ASSERT_TRUE(h.restart(3)) << h.last_error();
    EXPECT_NE(h.node(3).port, old_port) << "重启后端口应重新分配";
    for (int i = 0; i < 40 && !h.probe(3, 300); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    EXPECT_TRUE(h.probe(3)) << "restarted node not serving";
    /* 其余节点不受影响 */
    EXPECT_TRUE(h.probe(0));
    EXPECT_TRUE(h.probe(5));
}
