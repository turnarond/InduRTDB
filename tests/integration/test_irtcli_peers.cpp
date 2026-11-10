/**
 * @file test_irtcli_peers.cpp
 * @brief v3.12 M3.2 ④：irtcli SDK 与 CLI 的成员管控通道端到端验证。
 *
 * 真实 fork rtdbd + 策略文件，经 irtcli 公共 API 与 irtcli CLI 往返
 * OP_SET_PEERS(23) / OP_GET_PEERS(24)。判据：
 *   - set/get 往返字段一致；
 *   - cap 不足返回 IRTCLI_ERR_ARG 且不截断；
 *   - 非法成员被服务端拒绝且旧表不变；
 *   - CLI `peers set` / `peers get` 可运行并打印成员。
 */
#include <gtest/gtest.h>

#include <irtcli/client.h>

#include <fcntl.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <rtdbd/protocol.h> /* 仅测试端用于交叉校验公共头结构尺寸 */

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif
#ifndef IRTCLI_CLI_BIN
#define IRTCLI_CLI_BIN ""
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

    bool start(const std::string &tag)
    {
        sock = "/tmp/indurtdb_irtcli_peers_" + tag + ".sock";
        policy = tmp_file("irtcli_peers_policy_" + tag + ".txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("irtcli_peers_cfg_" + tag + ".yaml", "points: []\n");
        ::unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd", "--socket", sock.c_str(),
                  "--instance", ("irtcli_peers_" + tag).c_str(),
                  "--policy", policy.c_str(), "--config", config.c_str(),
                  "--max-points", "64", "--max-subs", "4", (char *)nullptr);
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

irtcli_peer_t peer(const char *addr, const char *node)
{
    irtcli_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.addr, sizeof(p.addr), "%s", addr);
    std::snprintf(p.node_id, sizeof(p.node_id), "%s", node);
    p.state = IRTCLI_PEER_ST_ACTIVE;
    return p;
}

bool connect_client(irtcli_t &c, const std::string &sock)
{
    if (irtcli_init(&c, sock.c_str(), 0, nullptr, nullptr) != IRTCLI_OK) return false;
    irtcli_set_async(&c, false);
    return irtcli_connect(&c) == IRTCLI_OK;
}

} // namespace

TEST(IrtcliPeers, PublicHeaderLayoutMatchesProtocol)
{
    /* 公共头不 include protocol.h，但布局必须逐字节一致（隔离 + 兼容同时成立） */
    EXPECT_EQ(sizeof(irtcli_peer_t), sizeof(rtdbd_peer_t));
    EXPECT_EQ(sizeof(irtcli_peer_t), 128u);
    EXPECT_EQ(IRTCLI_PEER_MAX, RTDBD_PEER_MAX);
}

TEST(IrtcliPeers, SetThenGetRoundtrip)
{
    Proc proc;
    ASSERT_TRUE(proc.start("roundtrip"));
    irtcli_t c;
    ASSERT_TRUE(connect_client(c, proc.sock));

    irtcli_peer_t peers[3] = {peer("10.4.0.1", "n1"), peer("10.4.0.2", "n2"),
                              peer("10.4.0.3", "n3")};
    EXPECT_EQ(irtcli_set_peers(&c, peers, 3), IRTCLI_OK);

    irtcli_peer_t out[IRTCLI_PEER_MAX];
    uint32_t n = 0;
    std::memset(out, 0, sizeof(out));
    ASSERT_EQ(irtcli_get_peers(&c, out, IRTCLI_PEER_MAX, &n), IRTCLI_OK);
    ASSERT_EQ(n, 3u);
    EXPECT_STREQ(out[0].addr, "10.4.0.1");
    EXPECT_STREQ(out[2].node_id, "n3");
    irtcli_close(&c);
}

TEST(IrtcliPeers, SmallCapRejectedWithoutTruncation)
{
    Proc proc;
    ASSERT_TRUE(proc.start("smallcap"));
    irtcli_t c;
    ASSERT_TRUE(connect_client(c, proc.sock));

    irtcli_peer_t peers[3] = {peer("10.5.0.1", "a"), peer("10.5.0.2", "b"),
                              peer("10.5.0.3", "c")};
    ASSERT_EQ(irtcli_set_peers(&c, peers, 3), IRTCLI_OK);

    irtcli_peer_t out[2];
    uint32_t n = 99u;
    std::memset(out, 0, sizeof(out));
    EXPECT_EQ(irtcli_get_peers(&c, out, 2, &n), IRTCLI_ERR_ARG);
    EXPECT_EQ(n, 0u); /* 不截断、不越界 */
    irtcli_close(&c);
}

TEST(IrtcliPeers, InvalidPeerRejectedAndOldTableKept)
{
    Proc proc;
    ASSERT_TRUE(proc.start("invalid"));
    irtcli_t c;
    ASSERT_TRUE(connect_client(c, proc.sock));

    irtcli_peer_t good = peer("10.6.0.1", "keep");
    ASSERT_EQ(irtcli_set_peers(&c, &good, 1), IRTCLI_OK);

    irtcli_peer_t bad[2] = {peer("10.6.0.9", "new"), peer("bogus", "bad")};
    EXPECT_NE(irtcli_set_peers(&c, bad, 2), IRTCLI_OK);

    irtcli_peer_t out[IRTCLI_PEER_MAX];
    uint32_t n = 0;
    ASSERT_EQ(irtcli_get_peers(&c, out, IRTCLI_PEER_MAX, &n), IRTCLI_OK);
    ASSERT_EQ(n, 1u);
    EXPECT_STREQ(out[0].addr, "10.6.0.1"); /* 旧表不变 */
    irtcli_close(&c);
}

TEST(IrtcliPeers, CliPeersSetGetIsRunnable)
{
    Proc proc;
    ASSERT_TRUE(proc.start("cli"));
    pid_t cli = fork();
    ASSERT_GE(cli, 0);
    if (cli == 0) {
        int fd = ::open("/tmp/irtcli_peers_cli.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); ::close(fd); }
        execl(IRTCLI_CLI_BIN, "irtcli", "--sock", proc.sock.c_str(),
              "peers", "set", "2", "10.7.0.1", "10.7.0.2",
              "peers", "get", (char *)nullptr);
        _exit(127);
    }
    int st = 0;
    waitpid(cli, &st, 0);
    EXPECT_EQ(WEXITSTATUS(st), 0) << "irtcli peers set/get 子进程退出码应为 0";
    FILE *f = std::fopen("/tmp/irtcli_peers_cli.out", "r");
    ASSERT_NE(f, nullptr);
    char buf[4096] = {0};
    size_t got = std::fread(buf, 1, sizeof(buf) - 1, f);
    (void)got;
    std::fclose(f);
    EXPECT_NE(std::strstr(buf, "10.7.0.1"), nullptr);
    EXPECT_NE(std::strstr(buf, "10.7.0.2"), nullptr);
}
