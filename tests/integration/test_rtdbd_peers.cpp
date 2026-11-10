/**
 * @file test_rtdbd_peers.cpp
 * @brief v3.12 M3.2 ②：UDS 尾部可加 opcode 23/24 的端到端验证。
 *
 * 判据：既有 22 个 opcode 与 libindurtdb.a ABI 零改动（由全量回归网锁定）；
 * SET_PEERS 整组替换语义（合法/自地址/非法/超限/失败旧表不变/幂等）与
 * IR4_PeersRuntime 纯逻辑一致；GET_PEERS 返回同一张表。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <signal.h>

#include <rtdbd/protocol.h>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {

std::string tmp_file(const std::string &name, const std::string &body)
{
    std::string p = "/tmp/" + name;
    FILE *f = std::fopen(p.c_str(), "w");
    if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
    return p;
}

struct Proc {
    std::string sock, policy, config, peers_file;
    pid_t pid = -1;
    ~Proc() { stop(); }

    bool start()
    {
        sock = "/tmp/indurtdb_peers_test.sock";
        policy = tmp_file("peers_policy.txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("peers_cfg.yaml", "points: []\n");
        ::unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            std::vector<char*> avc;
            avc.push_back(const_cast<char*>(RTDBD_BIN));
            avc.push_back(const_cast<char*>("--socket")); avc.push_back(const_cast<char*>(sock.c_str()));
            avc.push_back(const_cast<char*>("--instance")); avc.push_back(const_cast<char*>("peers_uds_test"));
            avc.push_back(const_cast<char*>("--policy")); avc.push_back(const_cast<char*>(policy.c_str()));
            avc.push_back(const_cast<char*>("--config")); avc.push_back(const_cast<char*>(config.c_str()));
            avc.push_back(const_cast<char*>("--max-points")); avc.push_back(const_cast<char*>("64"));
            avc.push_back(const_cast<char*>("--max-subs")); avc.push_back(const_cast<char*>("4"));
            if (!peers_file.empty()) {
                avc.push_back(const_cast<char*>("--peers-file")); avc.push_back(const_cast<char*>(peers_file.c_str()));
            }
            avc.push_back((char*)NULL);
            execv(RTDBD_BIN, avc.data());
            _exit(127);
        }
        for (int i = 0; i < 500; ++i) {
            struct stat st;
            if (::stat(sock.c_str(), &st) == 0) return true;
            usleep(10000);
        }
        return false;
    }
    void stop()
    {
        if (pid > 0) { kill(pid, SIGTERM); int s = 0; waitpid(pid, &s, 0); pid = -1; }
        ::unlink(sock.c_str());
    }
};

int connect_to(const std::string &sock)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    for (int i = 0; i < 200; ++i) {
        if (::connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        usleep(10000);
    }
    ::close(fd);
    return -1;
}

bool write_all(int fd, const void *p, size_t n)
{
    const char *b = (const char *)p;
    while (n > 0) {
        ssize_t w = ::write(fd, b, n);
        if (w <= 0) return false;
        b += w; n -= (size_t)w;
    }
    return true;
}

bool read_all(int fd, void *p, size_t n)
{
    char *b = (char *)p;
    while (n > 0) {
        ssize_t r = ::read(fd, b, n);
        if (r <= 0) return false;
        b += r; n -= (size_t)r;
    }
    return true;
}

/* 发一个 opcode 帧（12B 头 + 可选负载），收 12B 响应头 + 负载。 */
bool roundtrip(int fd, uint16_t op, const void *body, uint32_t body_len,
               rtdbd_resp_hdr_t &hdr, void *out, size_t out_cap, size_t &out_len)
{
    rtdbd_req_hdr_t req;
    std::memset(&req, 0, sizeof(req));
    req.magic = RTDBD_MAGIC;
    req.version = RTDBD_PROTO_VERSION;
    req.opcode = op;
    req.payload_len = body_len;
    if (!write_all(fd, &req, sizeof(req))) return false;
    if (body_len && !write_all(fd, body, body_len)) return false;
    if (!read_all(fd, &hdr, sizeof(hdr))) return false;
    out_len = 0;
    if (hdr.payload_len) {
        if (hdr.payload_len > out_cap) return false;
        if (!read_all(fd, out, hdr.payload_len)) return false;
        out_len = hdr.payload_len;
    }
    return true;
}

rtdbd_peer_t peer(const char *addr, const char *node)
{
    rtdbd_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.addr, sizeof(p.addr), "%s", addr);
    std::snprintf(p.node_id, sizeof(p.node_id), "%s", node);
    p.state = RTDBD_PEER_ST_ACTIVE;
    return p;
}

rtdbd_peers_resp_t make_set(std::initializer_list<rtdbd_peer_t> ps)
{
    rtdbd_peers_resp_t m;
    std::memset(&m, 0, sizeof(m));
    m.count = 0;
    for (const auto &p : ps) {
        if (m.count < RTDBD_PEER_MAX) m.peers[m.count++] = p;
    }
    return m;
}

} // namespace

TEST(PeersUds, SetThenGetRoundtripAndIdempotent)
{
    /* 服务端对非法请求会关连接；忽略 SIGPIPE 让 write 返回 EPIPE 而非杀进程 */
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    int fd = connect_to(proc.sock);
    ASSERT_GE(fd, 0);

    static rtdbd_peers_resp_t body;
    body = make_set({peer("10.0.0.1", "n1"), peer("10.0.0.2", "n2"),
                     peer("10.0.0.3", "n3")});
    rtdbd_resp_hdr_t h;
    size_t len = 0;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &body, sizeof(body), h, nullptr, 0, len));
    EXPECT_EQ(h.status, RTDBD_ST_OK);

    static rtdbd_peers_resp_t got;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got, sizeof(got), len));
    EXPECT_EQ(h.status, RTDBD_ST_OK);
    EXPECT_EQ(len, sizeof(rtdbd_peers_resp_t));
    ASSERT_EQ(got.count, 3u);
    EXPECT_STREQ(got.peers[0].addr, "10.0.0.1");
    EXPECT_STREQ(got.peers[2].node_id, "n3");

    /* 幂等：同集合重复 SET → GET 不变 */
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &body, sizeof(body), h, nullptr, 0, len));
    EXPECT_EQ(h.status, RTDBD_ST_OK);
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got, sizeof(got), len));
    EXPECT_EQ(got.count, 3u);
    ::close(fd);
}

TEST(PeersUds, DeferredPeerAddExecutesAtLoopTail)
{
    /* 端到端证明「延迟动作队列已接入主循环循环尾」且状态机 JOINING->ACTIVE 闭环：
     * SET_PEERS 回调只入队（PEER_ADD + TRIGGER_FULLSYNC），drain 在循环尾先落
     * JOINING 再推进 ACTIVE；请求故意携带 UNKNOWN（非终态）。
     * 若队列仍是死代码，整组替换会原样落库 → 读回 UNKNOWN；断言 ACTIVE 可证伪。 */
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    int fd = connect_to(proc.sock);
    ASSERT_GE(fd, 0);

    static rtdbd_peers_resp_t body;
    rtdbd_peer_t p = peer("10.9.0.1", "joining-node");
    p.state = RTDBD_PEER_ST_UNKNOWN;  /* 终态必须由延迟动作落地，而非请求原样落库 */
    body = make_set({p});

    rtdbd_resp_hdr_t h;
    size_t len = 0;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &body, sizeof(body), h, nullptr, 0, len));
    ASSERT_EQ(h.status, RTDBD_ST_OK);

    /* 等主循环循环尾 drain 执行（poll 周期 500ms）：ADD(JOINING) -> FULLSYNC(ACTIVE) */
    usleep(1500000);
    static rtdbd_peers_resp_t got;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got, sizeof(got), len));
    ASSERT_EQ(h.status, RTDBD_ST_OK);
    ASSERT_EQ(got.count, 1u);
    EXPECT_EQ(got.peers[0].state, RTDBD_PEER_ST_ACTIVE)
        << "延迟动作（ADD+FULLSYNC）未在循环尾闭环：状态机 JOINING->ACTIVE 未落地";
    ::close(fd);
}

TEST(PeersUds, InvalidSetRejectedAndOldTableUnchanged)
{
    Proc proc;
    ASSERT_TRUE(proc.start());
    int fd = connect_to(proc.sock);
    ASSERT_GE(fd, 0);

    static rtdbd_peers_resp_t good = make_set({peer("10.0.0.1", "n1")});
    static rtdbd_peers_resp_t bad = make_set({peer("10.0.0.5", "n5"), peer("bogus", "bad")});
    rtdbd_resp_hdr_t h;
    size_t len = 0;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &good, sizeof(good), h, nullptr, 0, len));
    ASSERT_EQ(h.status, RTDBD_ST_OK);

    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &bad, sizeof(bad), h, nullptr, 0, len));
    EXPECT_EQ(h.status, RTDBD_ST_BAD_REQUEST);

    static rtdbd_peers_resp_t got;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got, sizeof(got), len));
    EXPECT_EQ(h.status, RTDBD_ST_OK);
    ASSERT_EQ(got.count, 1u);
    EXPECT_STREQ(got.peers[0].addr, "10.0.0.1"); /* 旧表不变 */
    ::close(fd);
}

TEST(PeersUds, PeersFileLoadsAtStartup)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    proc.peers_file = tmp_file("peers_cfg.txt",
        "10.70.0.1 n1\n"
        "10.70.0.2 n2\n"
        "# comment\n");
    ASSERT_TRUE(proc.start());
    int fd = connect_to(proc.sock);
    ASSERT_GE(fd, 0);
    rtdbd_resp_hdr_t h;
    size_t len = 0;
    static rtdbd_peers_resp_t got;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got, sizeof(got), len));
    ASSERT_EQ(h.status, RTDBD_ST_OK);
    ASSERT_EQ(got.count, 2u);
    usleep(1500000);
    static rtdbd_peers_resp_t got2;
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, 0, h, &got2, sizeof(got2), len));
    ASSERT_EQ(got2.count, 2u);
    EXPECT_STREQ(got2.peers[0].addr, "10.70.0.1");
    EXPECT_EQ(got2.peers[0].state, RTDBD_PEER_ST_ACTIVE)
        << "--peers-file 初始成员未经循环尾延迟动作落地为 ACTIVE";
    ::close(fd);
}

TEST(PeersUds, PeersFileBadFormatRejected)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    proc.peers_file = tmp_file("peers_bad.txt", "not-an-ip n1\n");
    ASSERT_FALSE(proc.start());
}
