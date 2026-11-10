/**
 * @file test_sync_self_addr.cpp
 * @brief v3.12 M3.3：--sync-self-addr 接线后端到端验证（§197 拒绝自地址）。
 *
 * 真实 fork rtdbd 并传 --sync-self-addr，经 UDS 发 OP_SET_PEERS：
 *   含自地址 → BAD_REQUEST 且旧表不变；不含 → OK 且 GET 可读回。
 * 未接线时 rtdbd 会因未知参数退出 → 用例 RED（可证伪）。
 */
#include <gtest/gtest.h>
#include <rtdbd/protocol.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

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
    std::string sock, policy, config;
    pid_t pid = -1;
    ~Proc() { stop(); }
    bool start()
    {
        sock = "/tmp/indurtdb_self_addr.sock";
        policy = tmp_file("self_addr_policy.txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("self_addr_cfg.yaml", "points: []\n");
        ::unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd", "--socket", sock.c_str(),
                  "--instance", "self_addr_test", "--policy", policy.c_str(),
                  "--config", config.c_str(), "--max-points", "64", "--max-subs", "4",
                  "--sync-self-addr", "10.9.9.9", (char *)nullptr);
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
template <typename T> bool wr(int fd, const T &v)
{
    const char *p = (const char *)&v; size_t n = sizeof(T);
    while (n) { ssize_t w = ::write(fd, p, n); if (w <= 0) return false; p += w; n -= (size_t)w; }
    return true;
}
template <typename T> bool rd(int fd, T &v)
{
    char *p = (char *)&v; size_t n = sizeof(T);
    while (n) { ssize_t r = ::read(fd, p, n); if (r <= 0) return false; p += r; n -= (size_t)r; }
    return true;
}
bool roundtrip(int fd, uint16_t op, const rtdbd_peers_resp_t *body,
               rtdbd_resp_hdr_t &hdr, rtdbd_peers_resp_t &out)
{
    rtdbd_req_hdr_t req;
    std::memset(&req, 0, sizeof(req));
    req.magic = RTDBD_MAGIC; req.version = RTDBD_PROTO_VERSION;
    req.opcode = op; req.payload_len = body ? (uint32_t)sizeof(*body) : 0u;
    if (!wr(fd, req)) return false;
    if (body && !wr(fd, *body)) return false;
    if (!rd(fd, hdr)) return false;
    std::memset(&out, 0, sizeof(out));
    if (hdr.payload_len == 0) return true;
    if (hdr.payload_len != sizeof(out)) return false;
    return rd(fd, out);
}
rtdbd_peer_t peer(const char *addr)
{
    rtdbd_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.addr, sizeof(p.addr), "%s", addr);
    std::snprintf(p.node_id, sizeof(p.node_id), "%s", addr);
    p.state = RTDBD_PEER_ST_ACTIVE;
    return p;
}
}  // namespace

TEST(SyncSelfAddr, RejectsOwnAddressAndKeepsOldTable)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start()) << "rtdbd 未就绪（--sync-self-addr 未接线？）";
    int fd = connect_to(proc.sock);
    ASSERT_GE(fd, 0);

    static rtdbd_peers_resp_t set, out;
    rtdbd_resp_hdr_t h;
    std::memset(&set, 0, sizeof(set));
    set.count = 2;
    set.peers[0] = peer("10.1.1.1");
    set.peers[1] = peer("10.1.1.2");
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &set, h, out));
    ASSERT_EQ(h.status, RTDBD_ST_OK);

    std::memset(&set, 0, sizeof(set));
    set.count = 2;
    set.peers[0] = peer("10.2.2.2");
    set.peers[1] = peer("10.9.9.9");   /* 本节点地址 */
    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_SET_PEERS, &set, h, out));
    EXPECT_EQ(h.status, RTDBD_ST_BAD_REQUEST);

    ASSERT_TRUE(roundtrip(fd, RTDBD_OP_GET_PEERS, nullptr, h, out));
    ASSERT_EQ(h.status, RTDBD_ST_OK);
    ASSERT_EQ(out.count, 2u);
    EXPECT_STREQ(out.peers[0].addr, "10.1.1.1");
    EXPECT_STREQ(out.peers[1].addr, "10.1.1.2");
    ::close(fd);
}
