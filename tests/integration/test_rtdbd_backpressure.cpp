/**
 * @file test_rtdbd_backpressure.cpp
 * @brief rtdbd 背压（每连接有界出站队列）集成测试（B2，v3.7 主题B）
 *
 * 覆盖 B2 验收用例：
 *   - SlowConsumerDropsOldest   慢消费者（订阅不读）→ 队列填满 → notify_drop>0
 *   - FastConsumerUnaffected     慢消费者存在时，正常客户端读写与收通知不受影响
 *   - GracefulExitZero           SIGTERM → 退出码 0（限时排空后正常退出）
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <poll.h>
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

struct RtdbdProc {
    std::string sock;
    std::string stats_path;
    std::string instance;
    pid_t       pid = -1;

    ~RtdbdProc() { stop(); }

    bool start(const std::string& tag, const std::string& policy_body,
               const std::string& config_body)
    {
        sock        = "/tmp/indurtdb_bp_" + tag + ".sock";
        stats_path  = "/tmp/indurtdb_bp_" + tag + ".stats";
        instance    = "bp_test_" + tag;
        std::string policy_path = make_tmp_file("bp_policy_" + tag + ".txt", policy_body);
        std::string config_path = make_tmp_file("bp_cfg_" + tag + ".yaml", config_body);

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

    /* SIGTERM 并有界等待退出，返回退出码（-1 = 超时未退出）。
     * 有界等待 + SIGKILL 兜底：绝不让 CI 挂死在 waitpid 上。 */
    int term_and_wait(int timeout_ms)
    {
        if (pid <= 0) return -1;
        kill(pid, SIGTERM);
        for (int i = 0; i < timeout_ms / 10; ++i) {
            int st = 0;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) {
                pid = -1;
                return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            }
            usleep(10000);
        }
        return -1;   /* 超时（pid 保持有效，交由 stop() 兜底收尾） */
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
    "    name: \"BP_Point\"\n"
    "    type: int32\n"
    "    access: 3\n";

std::string policy_allow()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

bool raw_exchange(int fd, uint16_t op, const void* payload, uint32_t plen,
                  rtdbd_resp_hdr_t* resp, std::vector<uint8_t>* body)
{
    rtdbd_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic       = RTDBD_MAGIC;
    req.version     = RTDBD_PROTO_VERSION;
    req.opcode      = op;
    req.payload_len = plen;
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

bool subscribe(int fd, uint32_t id)
{
    rtdbd_sub_req_t s;
    memset(&s, 0, sizeof(s));
    s.point_id = id;
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_SUBSCRIBE, &s, sizeof(s), &resp, &body)) return false;
    return resp.status == RTDBD_ST_OK;
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

/* 统计收到的 NOTIFY 帧数（读到超时为止）。 */
int count_notify(int fd, int timeout_ms)
{
    int n = 0;
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    uint8_t buf[12 + 32];
    while (true) {
        if (poll(&pfd, 1, timeout_ms) <= 0) break;
        ssize_t got = 0;
        while (got < (ssize_t)sizeof(buf)) {
            ssize_t k = recv(fd, buf + got, sizeof(buf) - (size_t)got, 0);
            if (k <= 0) break;
            got += k;
        }
        if (got == (ssize_t)sizeof(buf)) n++;
        else break;
    }
    return n;
}

bool query_health(int fd, rtdbd_health_t* out)
{
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_HEALTH, nullptr, 0, &resp, &body)) return false;
    if (resp.status != RTDBD_ST_OK || body.size() != sizeof(rtdbd_health_t)) return false;
    memcpy(out, body.data(), sizeof(*out));
    return true;
}

} // namespace

/* 1. 慢消费者（订阅但不读）→ 队列填满 → 丢最旧，notify_drop > 0 */
TEST(Backpressure, SlowConsumerDropsOldest)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("slow", policy_allow(), kConfig));

    int fd_slow = connect_to(p.sock);
    ASSERT_GE(fd_slow, 0);
    /* 显式压小接收缓冲：让背压成为结构性前提，而非依赖宿主机
     * net.core.wmem_default 的调参（否则在缓冲较大的机器/CI 上可能不丢，flaky）。 */
    int rcvbuf = 2048;
    setsockopt(fd_slow, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    ASSERT_TRUE(subscribe(fd_slow, 10));
    /* 关键：此后**不读** fd_slow，让服务端出站队列被填满 */

    int fd_w = connect_to(p.sock);
    ASSERT_GE(fd_w, 0);

    /* 连写远超队列容量（64）与 socket 缓冲 → 触发丢最旧 */
    const int kWrites = 4000;
    for (int i = 1; i <= kWrites; ++i) {
        rtdbd_resp_hdr_t resp;
        ASSERT_TRUE(write_point(fd_w, 10, i, &resp)) << "write failed at " << i;
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    rtdbd_health_t h;
    ASSERT_TRUE(query_health(fd_w, &h));
    EXPECT_GT(h.notify_drop, 0u) << "slow consumer should cause drops";
    EXPECT_GT(h.n_notifies, 0u);

    close(fd_slow);
    close(fd_w);
    p.stop();
}

/* 2. 慢消费者存在时，正常客户端读写与收通知不受影响（无 head-of-line 阻塞） */
TEST(Backpressure, FastConsumerUnaffected)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("mixed", policy_allow(), kConfig));

    int fd_slow = connect_to(p.sock);
    ASSERT_GE(fd_slow, 0);
    ASSERT_TRUE(subscribe(fd_slow, 10));   /* 订阅但从不读 */

    int fd_fast = connect_to(p.sock);
    ASSERT_GE(fd_fast, 0);
    ASSERT_TRUE(subscribe(fd_fast, 10));

    int fd_w = connect_to(p.sock);
    ASSERT_GE(fd_w, 0);

    const int kWrites = 500;
    for (int i = 1; i <= kWrites; ++i) {
        rtdbd_resp_hdr_t resp;
        ASSERT_TRUE(write_point(fd_w, 10, i, &resp)) << "write failed at " << i;
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    /* 快速客户端应能收到通知（证明其未被慢消费者拖死） */
    int got = count_notify(fd_fast, 500);
    EXPECT_GT(got, 0) << "fast consumer should still receive notifications";

    rtdbd_health_t h;
    ASSERT_TRUE(query_health(fd_w, &h));
    EXPECT_GT(h.n_writes, 0u);

    close(fd_slow);
    close(fd_fast);
    close(fd_w);
    p.stop();
}

/* 3. SIGTERM → 退出码 0（限时排空后正常退出，不阻塞强退） */
TEST(Backpressure, GracefulExitZero)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("graceful", policy_allow(), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    rtdbd_resp_hdr_t resp;
    ASSERT_TRUE(write_point(fd, 10, 7, &resp));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    int code = p.term_and_wait(5000);
    EXPECT_EQ(code, 0) << "graceful exit should return 0";

    close(fd);
    p.stop();
}

/* 4. 帧结构完整性守卫：重背压下收到的每一帧都必须**完整可解析**。
 *    逐帧校验 magic / version / opcode / payload_len 与取值单调性，
 *    而不是只数 44B 块数（混合帧长度也是 44B，只数块数会漏）。
 *
 *    诚实边界：本用例**不是**「部分发送帧混合」缺陷的确定性复现。
 *    该缺陷需「发送缓冲剩余空间 >0 且 <44B」这一窄窗口才会触发；
 *    而在 MSG_DONTWAIT 下缓冲满时 send 直接返回 EAGAIN（0 字节），
 *    outq_sent 恒为 0。实测回退该修复后本用例仍通过，说明它抓不到该窗口。
 *    修复本身由代码评审确认（构造上禁止淘汰已部分上线的队首），
 *    若需确定性复现，须把 net.core.wmem_default 调到 <44B 量级属系统级变更，
 *    故此处保留为"帧结构完整性"回归守卫，不宣称覆盖该窄窗口。 */
TEST(Backpressure, FramesStayIntactUnderBackpressure)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("framing", policy_allow(), kConfig));

    int fd_slow = connect_to(p.sock);
    ASSERT_GE(fd_slow, 0);
    /* 压小接收缓冲，强制服务端经历"部分发送 + 队列积压" */
    int rcvbuf = 1024;
    setsockopt(fd_slow, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    ASSERT_TRUE(subscribe(fd_slow, 10));

    int fd_w = connect_to(p.sock);
    ASSERT_GE(fd_w, 0);

    const int kWrites = 2000;
    for (int i = 1; i <= kWrites; ++i) {
        rtdbd_resp_hdr_t resp;
        ASSERT_TRUE(write_point(fd_w, 10, i, &resp)) << "write failed at " << i;
        ASSERT_EQ(resp.status, RTDBD_ST_OK);
    }

    /* 慢读，逐帧校验结构完整性 */
    struct pollfd pfd;
    pfd.fd = fd_slow;
    pfd.events = POLLIN;
    int frames = 0, bad = 0;
    uint8_t buf[12 + 32];
    uint32_t last_value = 0;
    for (int i = 0; i < 2000; ++i) {
        if (poll(&pfd, 1, 50) <= 0) break;
        ssize_t got = 0;
        while (got < (ssize_t)sizeof(buf)) {
            ssize_t k = recv(fd_slow, buf + got, sizeof(buf) - (size_t)got, 0);
            if (k <= 0) break;
            got += k;
        }
        if (got != (ssize_t)sizeof(buf)) break;
        frames++;

        rtdbd_req_hdr_t h;
        memcpy(&h, buf, sizeof(h));
        rtdbd_notify_t nt;
        memcpy(&nt, buf + sizeof(h), sizeof(nt));

        if (h.magic != RTDBD_MAGIC) bad++;
        if (h.version != RTDBD_PROTO_VERSION) bad++;
        if (h.opcode != RTDBD_OP_NOTIFY) bad++;
        if (h.payload_len != sizeof(nt)) bad++;
        if (nt.point_id != 10) bad++;
        /* 值应单调递增（保序）：混合帧会破坏这一点 */
        uint32_t v = (uint32_t)(int32_t)nt.value_bits;
        if (v < last_value) bad++;
        last_value = v;
    }

    EXPECT_GT(frames, 0) << "should have received some frames";
    EXPECT_EQ(bad, 0) << "no mixed/corrupt frames allowed under backpressure";

    close(fd_slow);
    close(fd_w);
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
