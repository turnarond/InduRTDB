/**
 * @file test_rtdbd_core.cpp
 * @brief rtdbd 骨架集成测试（TDD 红阶段：先写失败测试）
 *
 * 覆盖 T1 的验收用例：
 *   - PingOk                      服务可连通
 *   - WriteSerializesConcurrentClients  多客户端并发写同一点位，均成功（无 busy）
 *   - RejectsUnauthorizedUid      策略外的 uid 被拒绝
 *   - AuditRecordsPidPointTs      审计记录 pid / point_id / ts
 *   - ProtocolVersionMismatchRejected  协议版本不匹配时拒绝（不静默）
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>

#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {

std::string g_test_dir;

std::string make_tmp_file(const std::string& name, const std::string& content)
{
    std::string path = "/tmp/" + name;
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
    }
    return path;
}

/* 启动 rtdbd 子进程（RAII：析构必定回收，避免 ASSERT 提前返回导致进程泄漏）
 *
 * 注意：泄漏的守护进程会继承测试进程的 stdout，导致 ctest 的输出管道永不关闭
 * 而永久等待。因此回收必须放在析构里，不能依赖测试体末尾的显式调用。
 */
struct RtdbdProc {
    std::string sock;
    std::string instance;
    std::string policy_path;
    pid_t       pid = -1;

    ~RtdbdProc() { stop(); }

    bool start(const std::string& tag, const std::string& policy_body)
    {
        sock     = "/tmp/indurtdb_rtdbd_" + tag + ".sock";
        instance = "rtdbd_test_" + tag;
        policy_path = make_tmp_file("rtdbd_policy_" + tag + ".txt", policy_body);

        unlink(sock.c_str());

        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy_path.c_str(),
                  "--max-points", "64",
                  "--max-subs", "4",
                  (char*)NULL);
            _exit(127);
        }

        /* 等待 socket 出现 */
        for (int i = 0; i < 500; ++i) {
            struct stat st;
            if (stat(sock.c_str(), &st) == 0) return true;
            usleep(10000);
        }
        return false;
    }

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

int connect_to(const std::string& sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

    for (int i = 0; i < 200; ++i) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        usleep(10000);
    }
    close(fd);
    return -1;
}

bool send_req(int fd, uint16_t opcode, const void* payload, uint32_t len)
{
    rtdbd_req_hdr_t h;
    h.magic       = RTDBD_MAGIC;
    h.version     = RTDBD_PROTO_VERSION;
    h.opcode      = opcode;
    h.payload_len = len;

    if (send(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) return false;
    if (len > 0 && payload) {
        if (send(fd, payload, len, 0) != (ssize_t)len) return false;
    }
    return true;
}

bool recv_resp(int fd, rtdbd_resp_hdr_t* out)
{
    return recv(fd, out, sizeof(*out), MSG_WAITALL) == (ssize_t)sizeof(*out);
}

std::string policy_for_uid(unsigned long uid)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", uid);
    return std::string(buf);
}

} // namespace

/* 1. 服务可连通 */
TEST(RtdbdCore, PingOk)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("ping", policy_for_uid((unsigned long)getuid())));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    ASSERT_TRUE(send_req(fd, RTDBD_OP_PING, NULL, 0));
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(recv_resp(fd, &resp));
    EXPECT_EQ(resp.magic, RTDBD_MAGIC);
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    close(fd);
    p.stop();
}

/* 2. 多客户端并发写同一点位，均成功且无 busy */
TEST(RtdbdCore, WriteSerializesConcurrentClients)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("serial", policy_for_uid((unsigned long)getuid())));

    int a = connect_to(p.sock);
    int b = connect_to(p.sock);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);

    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id    = 7;
    w.type        = RTDBD_TYPE_INT32;
    w.value_bits  = 1234;
    w.source_ts_ns = 0;

    ASSERT_TRUE(send_req(a, RTDBD_OP_WRITE, &w, sizeof(w)));
    ASSERT_TRUE(send_req(b, RTDBD_OP_WRITE, &w, sizeof(w)));

    rtdbd_resp_hdr_t ra, rb;
    ASSERT_TRUE(recv_resp(a, &ra));
    ASSERT_TRUE(recv_resp(b, &rb));

    EXPECT_EQ(ra.status, RTDBD_ST_OK) << "client A 写入应成功（串行化后不应出现 busy）";
    EXPECT_EQ(rb.status, RTDBD_ST_OK) << "client B 写入应成功（串行化后不应出现 busy）";

    close(a);
    close(b);
    p.stop();
}

/* 3. 策略外的 uid 被拒绝（deny by default） */
TEST(RtdbdCore, RejectsUnauthorizedUid)
{
    /* 策略文件为空 → 任何 uid 都不在规则内 → 拒绝 */
    RtdbdProc p;
    ASSERT_TRUE(p.start("deny", "# no rules\n"));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id   = 7;
    w.type       = RTDBD_TYPE_INT32;
    w.value_bits = 42;

    ASSERT_TRUE(send_req(fd, RTDBD_OP_WRITE, &w, sizeof(w)));
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(recv_resp(fd, &resp));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    close(fd);
    p.stop();
}

/* 4. 审计记录 pid / point_id / ts */
TEST(RtdbdCore, AuditRecordsPidPointTs)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("audit", policy_for_uid((unsigned long)getuid())));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id   = 11;
    w.type       = RTDBD_TYPE_INT32;
    w.value_bits = 99;

    ASSERT_TRUE(send_req(fd, RTDBD_OP_WRITE, &w, sizeof(w)));
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(recv_resp(fd, &resp));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 拉取审计 */
    ASSERT_TRUE(send_req(fd, RTDBD_OP_AUDIT_DUMP, NULL, 0));
    rtdbd_resp_hdr_t aresp;
    ASSERT_TRUE(recv_resp(fd, &aresp));
    ASSERT_EQ(aresp.status, RTDBD_ST_OK);
    ASSERT_GE(aresp.payload_len, sizeof(rtdbd_audit_entry_t));

    rtdbd_audit_entry_t entry;
    ASSERT_TRUE(recv(fd, &entry, sizeof(entry), MSG_WAITALL) == (ssize_t)sizeof(entry));

    EXPECT_EQ(entry.point_id, 11u);
    EXPECT_GT(entry.pid, 0u);
    EXPECT_GT(entry.ts_ns, 0u);

    close(fd);
    p.stop();
}

/* 5. 协议版本不匹配：不静默，直接断开 */
TEST(RtdbdCore, ProtocolVersionMismatchRejected)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("ver", policy_for_uid((unsigned long)getuid())));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_req_hdr_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.magic   = RTDBD_MAGIC;
    bad.version = RTDBD_PROTO_VERSION + 1; /* 版本不符 */
    bad.opcode  = RTDBD_OP_PING;
    ASSERT_EQ(send(fd, &bad, sizeof(bad), 0), (ssize_t)sizeof(bad));

    /* 服务端应关闭连接：recv 返回 0 */
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0) << "版本不匹配时服务端应关闭连接，而非返回响应";

    close(fd);
    p.stop();
}

/* 6. 写入携带采集时刻（SourceTimestamp）—— rtdbd 须将其写入 shm */
TEST(RtdbdCore, WriteCarriesSourceTimestamp)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("srcts", policy_for_uid((unsigned long)getuid())));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id = 5;
    w.type     = RTDBD_TYPE_DOUBLE;
    double v   = 21.5;
    memcpy(&w.value_bits, &v, sizeof(v));
    w.source_ts_ns = 1700000000123456789ull;

    ASSERT_TRUE(send_req(fd, RTDBD_OP_WRITE, &w, sizeof(w)));
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(recv_resp(fd, &resp));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    close(fd);

    /* 从共享内存侧读取，确认采集时刻已落地 */
    ASSERT_EQ(indurtdb_initialize(p.instance.c_str(), 64, 4), 0);
    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(5, &pt), 0);
    EXPECT_EQ(pt.source_timestamp_ns, 1700000000123456789ull);
    EXPECT_DOUBLE_EQ(pt.value.d, 21.5);
    indurtdb_shutdown();

    p.stop();
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
