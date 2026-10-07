/**
 * @file test_rtdbd_health.cpp
 * @brief rtdbd 健康 / 错误计数 + 进程自检集成测试（B1，v3.7 主题B）
 *
 * 覆盖 B1 验收用例：
 *   - HealthOkAfterWrites       写成功后 HEALTH 返回 OK 且 n_writes 合理
 *   - Sigusr1DumpWritesFile     SIGUSR1 → stats 文件生成且含 counters
 *   - HealthDegradedAfterDeny   注入 policy deny → write_rejected>0 且 DEGRADED
 *   - HealthRejectsBadPayload   HEALTH 带负载 → BAD_REQUEST 且 decode_fail 递增
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {

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

/* 启动 rtdbd 子进程（RAII）。stats_path 用于 SIGUSR1 dump 落盘。 */
struct RtdbdProc {
    std::string sock;
    std::string instance;
    std::string stats_path;
    pid_t       pid = -1;

    ~RtdbdProc() { stop(); }

    bool start(const std::string& tag, const std::string& policy_body,
               const std::string& config_body)
    {
        sock        = "/tmp/indurtdb_health_" + tag + ".sock";
        instance    = "health_test_" + tag;
        stats_path  = "/tmp/indurtdb_health_" + tag + ".stats";
        std::string policy_path = make_tmp_file("health_policy_" + tag + ".txt", policy_body);
        std::string config_path = make_tmp_file("health_cfg_" + tag + ".yaml", config_body);

        unlink(sock.c_str());
        unlink(stats_path.c_str());

        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy_path.c_str(),
                  "--config", config_path.c_str(),
                  "--stats-file", stats_path.c_str(),
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

    void stop()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int reaped = 0;
            for (int i = 0; i < 500; ++i) {          /* 最多 5s */
                int st = 0;
                if (waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
                usleep(10000);
            }
            if (!reaped) {                            /* 卡死则 SIGKILL 强退 */
                kill(pid, SIGKILL);
                int st = 0;
                waitpid(pid, &st, 0);
            }
            pid = -1;
        }
        unlink(sock.c_str());
        unlink(stats_path.c_str());
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

const char* kConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"Health_Point\"\n"
    "    type: int32\n"
    "    access: 3\n";

/* 允许当前 uid 操作点 0..63（写测试用）；deny 策略在单独用例里构造 */
std::string policy_allow()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

/* 空策略 = deny by default（所有写被拒） */
std::string policy_deny_all()
{
    return std::string("");
}

bool raw_exchange(int fd, uint16_t op, const void* payload, uint32_t plen,
                  rtdbd_resp_hdr_t* resp, std::vector<uint8_t>* body)
{
    rtdbd_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic        = RTDBD_MAGIC;
    req.version      = RTDBD_PROTO_VERSION;
    req.opcode       = op;
    req.payload_len  = plen;
    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) return false;
    if (plen && send(fd, payload, plen, 0) != (ssize_t)plen) return false;

    if (recv(fd, resp, sizeof(*resp), 0) != (ssize_t)sizeof(*resp)) return false;
    body->resize(resp->payload_len);
    size_t got = 0;
    while (got < resp->payload_len) {
        ssize_t n = recv(fd, body->data() + got, resp->payload_len - got, 0);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

bool write_point(int fd, uint32_t id, int32_t v, rtdbd_resp_hdr_t* resp)
{
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id   = id;
    w.type       = (uint8_t)INDURTDB_TYPE_INT32;
    w.value_bits = (uint64_t)(int32_t)v;
    std::vector<uint8_t> body;
    return raw_exchange(fd, RTDBD_OP_WRITE, &w, sizeof(w), resp, &body);
}

bool query_health(int fd, rtdbd_health_t* out)
{
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_HEALTH, nullptr, 0, &resp, &body)) return false;
    if (resp.status != RTDBD_ST_OK) return false;
    if (body.size() != sizeof(rtdbd_health_t)) return false;
    memcpy(out, body.data(), sizeof(*out));
    return true;
}

} // namespace

/* 1. 写成功后 HEALTH 返回 OK，n_writes 计数合理 */
TEST(Health, OkAfterWrites)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("ok", policy_allow(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    for (int i = 1; i <= 3; ++i) {
        rtdbd_resp_hdr_t resp;
        ASSERT_TRUE(write_point(fd, 10, i, &resp));
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    rtdbd_health_t h;
    ASSERT_TRUE(query_health(fd, &h));
    EXPECT_EQ(h.n_writes, 3u);
    EXPECT_EQ(h.health, (uint32_t)RTDBD_HEALTH_OK);
    EXPECT_GT(h.uptime_ns, 0u);

    close(fd);
    p.stop();
}

/* 2. n_notifies 语义钉定：入队计数（= 广播次数，不扣 notify_drop）。
 * 评审发现头文件注释曾写「成功发出且扣除丢弃」，与实现不符 —— 此用例钉住实现语义。 */
TEST(Health, NotifiesCountsEnqueues)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("ncount", policy_allow(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    /* 订阅连接独立于写连接：B2 起通知是异步推送，若与请求共处同一连接，
     * 响应流会与 NOTIFY 帧交织（raw_exchange 会把推送帧误当响应头解析）。 */
    int fd_sub = connect_to(p.sock);
    ASSERT_GE(fd_sub, 0);
    rtdbd_sub_req_t s; memset(&s, 0, sizeof(s)); s.point_id = 10;
    {
        rtdbd_resp_hdr_t resp; std::vector<uint8_t> body;
        ASSERT_TRUE(raw_exchange(fd_sub, RTDBD_OP_SUBSCRIBE, &s, sizeof(s), &resp, &body));
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    for (int i = 1; i <= 5; ++i) {
        rtdbd_resp_hdr_t resp;
        ASSERT_TRUE(write_point(fd, 10, i, &resp));
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    rtdbd_health_t h;
    ASSERT_TRUE(query_health(fd, &h));
    EXPECT_EQ(h.n_notifies, 5u) << "n_notifies = enqueue count (broadcasts)";
    EXPECT_EQ(h.notify_drop, 0u) << "no drops expected for a reading client";

    close(fd);
    close(fd_sub);
    p.stop();
}

/* 3. SIGUSR1 → stats 文件生成且含 counters 字段 */
TEST(Health, Sigusr1DumpWritesFile)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("dump", policy_allow(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(write_point(fd, 10, 42, &resp));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 触发 dump：SIGUSR1 → 主循环写文件 */
    ASSERT_EQ(kill(p.pid, SIGUSR1), 0);

    /* 等文件出现（主循环 poll 最多 500ms 一轮） */
    std::string content;
    for (int i = 0; i < 200; ++i) {
        FILE* f = fopen(p.stats_path.c_str(), "r");
        if (f) {
            char buf[4096] = {0};
            size_t n = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            content.assign(buf, n);
            if (!content.empty()) break;
        }
        usleep(10000);
    }

    EXPECT_NE(content.find("n_writes"), std::string::npos) << content;
    EXPECT_NE(content.find("write_rejected"), std::string::npos) << content;
    EXPECT_NE(content.find("health"), std::string::npos) << content;
    EXPECT_NE(content.find("n_conns"), std::string::npos) << content;

    close(fd);
    p.stop();
}

/* 3. 注入 policy deny（写被拒）→ write_rejected>0 且 DEGRADED */
TEST(Health, DegradedAfterDeny)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("deny", policy_deny_all(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 空策略 → deny by default，写必被拒 */
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(write_point(fd, 10, 1, &resp));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    rtdbd_health_t h;
    ASSERT_TRUE(query_health(fd, &h));
    EXPECT_GT(h.write_rejected, 0u);
    EXPECT_EQ(h.health, (uint32_t)RTDBD_HEALTH_DEGRADED);

    close(fd);
    p.stop();
}

/* 4. HEALTH 带非法负载 → BAD_REQUEST，且 decode_fail 递增 */
TEST(Health, RejectsBadPayload)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("badpayload", policy_allow(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_health_t before;
    ASSERT_TRUE(query_health(fd, &before));

    /* HEALTH 期望零负载，喂 4B → BAD_REQUEST */
    uint32_t junk = 0;
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_HEALTH, &junk, sizeof(junk), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_BAD_REQUEST);

    /* 换新连接查 decode_fail 已递增（坏负载那条连接被服务端关闭） */
    int fd2 = connect_to(p.sock);
    ASSERT_GE(fd2, 0);
    rtdbd_health_t after;
    ASSERT_TRUE(query_health(fd2, &after));
    EXPECT_GT(after.decode_fail, before.decode_fail);

    close(fd);
    close(fd2);
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
