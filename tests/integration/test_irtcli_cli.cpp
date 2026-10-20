/**
 * @file test_irtcli_cli.cpp
 * @brief irtcli CLI 分区支持集成测试（v3.9 T6 红→绿）
 *
 * 覆盖 T6 验收用例：
 *   - Cli.PartitionFlag        --partition N 决定 find 使用哪个分区
 *   - Cli.SelectCommand        select N 影响后续命令的默认分区
 *   - Cli.ListPartitionFilter  list 只列当前默认分区的点位
 *   - Cli.HelpShowsPartition   --help 反映 --partition 与 select
 *
 * 起服务骨架复用 test_rtdbd_proto_v3.cpp 的 RtdbdProc（fork+exec rtdbd + 等 socket）。
 * 配置点位恒落分区 0（核心配置无分区字段，见 indurtdb.c），故分区 1 的点位
 * 在运行期通过裸协议 CREATE_POINT 创建，构造「同名跨分区」与「仅分区 1」场景。
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

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
#ifndef IRTCLI_BIN
#define IRTCLI_BIN ""
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

/* 启动 rtdbd 子进程（RAII：析构必定回收，避免 ASSERT 提前返回导致进程泄漏） */
struct RtdbdProc {
    std::string sock;
    std::string instance;
    std::string policy_path;
    std::string config_path;
    pid_t       pid = -1;

    ~RtdbdProc() { stop(); }

    bool start(const std::string& tag, const std::string& policy_body,
               const std::string& config_body)
    {
        sock        = "/tmp/indurtdb_irtcli_cli_" + tag + ".sock";
        instance    = "irtcli_cli_test_" + tag;
        policy_path = make_tmp_file("irtcli_cli_policy_" + tag + ".txt", policy_body);
        config_path = make_tmp_file("irtcli_cli_cfg_" + tag + ".yaml", config_body);

        unlink(sock.c_str());

        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy_path.c_str(),
                  "--config", config_path.c_str(),
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

std::string policy_for_self()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:*:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

/* 裸协议交互：发请求头+负载，收响应头+全部负载体。返回是否成功收完。 */
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

/* 运行期创建一个点位（用于构造分区 1 的点位，因配置点位恒落分区 0）。 */
bool create_point(int fd, uint32_t id, uint32_t partition, const char* name, uint8_t type)
{
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id  = id;
    c.type      = type;
    c.access    = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c.name, name, sizeof(c.name) - 1);
    c.partition = partition;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body)) return false;
    return resp.status == RTDBD_ST_OK;
}

struct CliResult {
    int         rc = -1;
    std::string out;
    std::string err;
};

/* fork+exec 跑 CLI，用管道捕获 stdout/stderr 与退出码。 */
CliResult run_cli(const std::vector<std::string>& args)
{
    CliResult r;
    int op[2] = { -1, -1 };
    int ep[2] = { -1, -1 };
    if (pipe(op) != 0) return r;
    if (pipe(ep) != 0) { close(op[0]); close(op[1]); return r; }

    pid_t pid = fork();
    if (pid < 0) {
        close(op[0]); close(op[1]); close(ep[0]); close(ep[1]);
        return r;
    }
    if (pid == 0) {
        dup2(op[1], STDOUT_FILENO);
        dup2(ep[1], STDERR_FILENO);
        close(op[0]); close(op[1]);
        close(ep[0]); close(ep[1]);

        std::vector<char*> cargv;
        cargv.reserve(args.size() + 1);
        for (size_t i = 0; i < args.size(); ++i)
            cargv.push_back(const_cast<char*>(args[i].c_str()));
        cargv.push_back(NULL);

        execv(cargv[0], cargv.data());
        _exit(127);
    }

    close(op[1]);
    close(ep[1]);

    char buf[4096];
    ssize_t n;
    while ((n = read(op[0], buf, sizeof(buf))) > 0) r.out.append(buf, (size_t)n);
    while ((n = read(ep[0], buf, sizeof(buf))) > 0) r.err.append(buf, (size_t)n);
    close(op[0]);
    close(ep[0]);

    int st = 0;
    if (waitpid(pid, &st, 0) == pid && WIFEXITED(st)) r.rc = WEXITSTATUS(st);
    return r;
}

/* 配置点位恒落分区 0：temp(id10)、flow(id11，仅分区 0)。 */
const char* kConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"temp\"\n"
    "    type: int32\n"
    "    unit: 0\n"
    "    access: 3\n"
    "  - id: 11\n"
    "    name: \"flow\"\n"
    "    type: int32\n"
    "    unit: 0\n"
    "    access: 3\n";

/* 建立一个含「同名跨分区」与「仅分区 1」点位的服务：
 *   分区 0: temp(10), flow(11)   ← 配置
 *   分区 1: temp(20), press(21)  ← 运行期创建 */
bool setup_service(RtdbdProc* p, const std::string& tag)
{
    if (!p->start(tag, policy_for_self(), kConfig)) return false;

    int fd = connect_to(p->sock);
    if (fd < 0) return false;
    bool ok = create_point(fd, 20, 1, "temp", RTDBD_TYPE_INT32) &&
              create_point(fd, 21, 1, "press", RTDBD_TYPE_INT32);
    close(fd);
    return ok;
}

} // namespace

/* 1. --partition N 决定 find 使用的分区（同名跨分区解析到不同 id）。 */
TEST(Cli, PartitionFlag)
{
    RtdbdProc p;
    ASSERT_TRUE(setup_service(&p, "partflag"));

    CliResult r1 = run_cli({ IRTCLI_BIN, "--sock", p.sock, "--partition", "1", "find", "temp" });
    ASSERT_EQ(r1.rc, 0) << "stderr: " << r1.err;
    EXPECT_EQ(std::stoul(r1.out), 20u) << "stdout: " << r1.out;

    CliResult r0 = run_cli({ IRTCLI_BIN, "--sock", p.sock, "--partition", "0", "find", "temp" });
    ASSERT_EQ(r0.rc, 0) << "stderr: " << r0.err;
    EXPECT_EQ(std::stoul(r0.out), 10u) << "stdout: " << r0.out;

    p.stop();
}

/* 2. select N 子命令影响后续命令的默认分区。 */
TEST(Cli, SelectCommand)
{
    RtdbdProc p;
    ASSERT_TRUE(setup_service(&p, "selectcmd"));

    CliResult r = run_cli({ IRTCLI_BIN, "--sock", p.sock, "select", "1", "find", "temp" });
    ASSERT_EQ(r.rc, 0) << "stderr: " << r.err;
    EXPECT_EQ(std::stoul(r.out), 20u) << "stdout: " << r.out;

    /* select 0 后再 find → 回到分区 0 的 temp */
    CliResult r2 = run_cli({ IRTCLI_BIN, "--sock", p.sock, "select", "1", "select", "0", "find", "temp" });
    ASSERT_EQ(r2.rc, 0) << "stderr: " << r2.err;
    EXPECT_EQ(std::stoul(r2.out), 10u) << "stdout: " << r2.out;

    p.stop();
}

/* 3. list 只列当前默认分区的点位。 */
TEST(Cli, ListPartitionFilter)
{
    RtdbdProc p;
    ASSERT_TRUE(setup_service(&p, "listfilter"));

    CliResult r = run_cli({ IRTCLI_BIN, "--sock", p.sock, "--partition", "1", "list" });
    ASSERT_EQ(r.rc, 0) << "stderr: " << r.err;

    std::istringstream is(r.out);
    std::string line;
    int count = 0;
    bool saw_press = false, saw_flow = false;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        std::istringstream ls(line);
        uint32_t id = 0, part = 0;
        std::string name;
        ASSERT_TRUE((bool)(ls >> id >> part >> name)) << "坏行: " << line;
        EXPECT_EQ(part, 1u) << "分区 1 的 list 不应含其它分区的点: " << line;
        if (name == "press") saw_press = true;
        if (name == "flow")  saw_flow  = true;
        ++count;
    }
    EXPECT_TRUE(saw_press) << "list 分区 1 应含 press";
    EXPECT_FALSE(saw_flow) << "list 分区 1 不应含仅分区 0 的 flow";
    EXPECT_EQ(count, 2) << "分区 1 恰含 temp、press 两点";

    p.stop();
}

/* 4. --help 反映新参数与子命令。 */
TEST(Cli, HelpShowsPartition)
{
    CliResult r = run_cli({ IRTCLI_BIN, "--help" });
    ASSERT_EQ(r.rc, 0) << "stderr: " << r.err;
    EXPECT_NE(r.out.find("--partition"), std::string::npos) << "help 应含 --partition\n" << r.out;
    EXPECT_NE(r.out.find("select"), std::string::npos) << "help 应含 select\n" << r.out;
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (std::string(RTDBD_BIN).empty()) {
        fprintf(stderr, "RTDBD_BIN not defined\n");
        return 1;
    }
    if (std::string(IRTCLI_BIN).empty()) {
        fprintf(stderr, "IRTCLI_BIN not defined\n");
        return 1;
    }
    return RUN_ALL_TESTS();
}
