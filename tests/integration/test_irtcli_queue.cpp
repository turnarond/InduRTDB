/**
 * @file test_irtcli_queue.cpp
 * @brief indurtdb-client 与写队列集成测试（T3 红阶段）
 *
 * 覆盖：
 *   - SyncWriteWaitsAck            同步写等待服务端确认并落 shm
 *   - AsyncWriteReturnsImmediately 异步写入队即返回，不阻塞
 *   - CoalescesSamePoint           同点位合并去重，只保留最新值
 *   - FlushAppliesQueuedWrites     队列按序提交到 shm
 *   - AlertsOnQueueFull            队列满触发告警，绝不静默丢弃
 *   - ReplayCarriesSourceTimestamp 重放携带原始采集时刻
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <indurtdb/indurtdb.h>
#include <irtcli/client.h>
#include <rtdbd/protocol.h>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {

int  g_alerts = 0;

void alert_cb(const char* msg, void* user)
{
    (void)msg;
    (void)user;
    ++g_alerts;
}

struct Proc {
    std::string sock;
    std::string instance;
    std::string policy;
    pid_t       pid = -1;

    bool start(const std::string& tag, const std::string& policy_body)
    {
        sock     = "/tmp/indurtdb_irtcli_" + tag + ".sock";
        instance = "irtcli_test_" + tag;
        policy   = "/tmp/irtcli_policy_" + tag + ".txt";

        FILE* f = fopen(policy.c_str(), "w");
        if (f) { fwrite(policy_body.data(), 1, policy_body.size(), f); fclose(f); }

        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy.c_str(),
                  "--max-points", "64",
                  "--max-subs", "4",
                  (char*)NULL);
            _exit(127);
        }
        for (int i = 0; i < 500; ++i) {
            struct stat st;
            if (stat(sock.c_str(), &st) == 0) return true;
            usleep(10000);
        }
        return false;
    }

    ~Proc() { stop(); }

    void stop()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
            pid = -1;
        }
        unlink(sock.c_str());
    }
};

std::string policy_for_self()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

/* 从共享内存侧读取点位（独立于客户端，验证真的落盘） */
bool shm_read_int32(const std::string& instance, uint32_t id, int32_t* out)
{
    if (indurtdb_initialize(instance.c_str(), 64, 4) != 0) return false;
    int32_t v = 0;
    bool ok = (indurtdb_read_int32(id, &v) == 0);
    if (ok && out) *out = v;
    indurtdb_shutdown();
    return ok;
}

bool shm_read_double(const std::string& instance, uint32_t id, double* out)
{
    if (indurtdb_initialize(instance.c_str(), 64, 4) != 0) return false;
    double v = 0;
    bool ok = (indurtdb_read_double(id, &v) == 0);
    if (ok && out) *out = v;
    indurtdb_shutdown();
    return ok;
}

uint64_t shm_source_ts(const std::string& instance, uint32_t id)
{
    if (indurtdb_initialize(instance.c_str(), 64, 4) != 0) return 0;
    indurtdb_point_t p;
    uint64_t ts = 0;
    if (indurtdb_read_point(id, &p) == 0) ts = p.source_timestamp_ns;
    indurtdb_shutdown();
    return ts;
}

} // namespace

TEST(IrtcliQueue, SyncWriteWaitsAck)
{
    Proc p;
    ASSERT_TRUE(p.start("sync", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, alert_cb, NULL), IRTCLI_OK);
    irtcli_set_async(&c, false);

    EXPECT_EQ(irtcli_write_int32(&c, 3, 42, 0), IRTCLI_OK);

    int32_t v = 0;
    ASSERT_TRUE(shm_read_int32(p.instance, 3, &v));
    EXPECT_EQ(v, 42);

    irtcli_close(&c);
}

TEST(IrtcliQueue, AsyncWriteReturnsImmediately)
{
    Proc p;
    ASSERT_TRUE(p.start("async", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, alert_cb, NULL), IRTCLI_OK);

    EXPECT_EQ(irtcli_write_int32(&c, 4, 7, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_queue_count(&c), 1u);

    irtcli_close(&c);
}

TEST(IrtcliQueue, CoalescesSamePoint)
{
    Proc p;
    ASSERT_TRUE(p.start("coalesce", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, alert_cb, NULL), IRTCLI_OK);

    EXPECT_EQ(irtcli_write_int32(&c, 5, 1, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 5, 2, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 5, 3, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_queue_count(&c), 1u) << "同点位应合并去重，只保留最新值";

    EXPECT_EQ(irtcli_flush(&c), 1);
    EXPECT_EQ(irtcli_queue_count(&c), 0u);

    int32_t v = 0;
    ASSERT_TRUE(shm_read_int32(p.instance, 5, &v));
    EXPECT_EQ(v, 3) << "合并后应提交最新值";

    irtcli_close(&c);
}

TEST(IrtcliQueue, FlushAppliesQueuedWrites)
{
    Proc p;
    ASSERT_TRUE(p.start("flush", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, alert_cb, NULL), IRTCLI_OK);

    EXPECT_EQ(irtcli_write_int32(&c, 6, 11, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 7, 22, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_queue_count(&c), 2u);

    EXPECT_EQ(irtcli_flush(&c), 2);

    int32_t a = 0, b = 0;
    ASSERT_TRUE(shm_read_int32(p.instance, 6, &a));
    ASSERT_TRUE(shm_read_int32(p.instance, 7, &b));
    EXPECT_EQ(a, 11);
    EXPECT_EQ(b, 22);

    irtcli_close(&c);
}

TEST(IrtcliQueue, AlertsOnQueueFull)
{
    Proc p;
    ASSERT_TRUE(p.start("full", policy_for_self()));

    g_alerts = 0;
    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 2, alert_cb, NULL), IRTCLI_OK);

    EXPECT_EQ(irtcli_write_int32(&c, 1, 1, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 2, 2, 0), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 3, 3, 0), IRTCLI_ERR_FULL) << "容量 2，第三个不同点位应失败";
    EXPECT_EQ(g_alerts, 1) << "队列满必须触发告警，不得静默丢弃";

    irtcli_close(&c);
}

TEST(IrtcliQueue, ReplayCarriesSourceTimestamp)
{
    Proc p;
    ASSERT_TRUE(p.start("srcts", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, alert_cb, NULL), IRTCLI_OK);

    const uint64_t src = 1700000000123456789ull;
    EXPECT_EQ(irtcli_write_double(&c, 8, 21.5, src), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_flush(&c), 1);

    EXPECT_EQ(shm_source_ts(p.instance, 8), src) << "重放必须携带原始采集时刻";

    double d = 0;
    ASSERT_TRUE(shm_read_double(p.instance, 8, &d));
    EXPECT_DOUBLE_EQ(d, 21.5);

    irtcli_close(&c);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (std::string(RTDBD_BIN).empty()) {
        fprintf(stderr, "RTDBD_BIN not defined\n");
        return 1;
    }
    return RUN_ALL_TESTS();
}
