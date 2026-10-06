/**
 * @file test_rtdbd_config.cpp
 * @brief rtdbd 配置校验 + fail-fast 集成测试（B3，v3.7 主题B）
 *
 * 覆盖 B3 验收用例：
 *   - ValidConfigStarts           合法配置（含点位语义）正常启动
 *   - MissingConfigExits2         配置文件不存在 → 退出码 2
 *   - BadEurRangeExits2           EUR 上下限颠倒 → 退出码 2
 *   - UnknownFlagsExits2          meta flags 含未知位 → 退出码 2
 *   - PercentDeadbandNoEurExits2 百分比死区但未启用量程 → 退出码 2
 *   - BadInstanceExits1           非法 --instance 字符 → 退出码 1
 *   - ZeroMaxPointsExits1         --max-points 0 → 退出码 1
 *   - OversizeMaxSubsExits1      --max-subs 超上限 → 退出码 1
 *
 * 注：非法点位语义经 YAML 可选字段（eur_min/eur_max/deadband/flags）表达，
 *     由 indurtdb_load_config 写入元数据区，再由启动校验拦下。
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/mman.h>
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

/* 运行 rtdbd 到退出，返回退出码；同时捕获 stderr。
 * 带超时保护：超时则 kill 并返回 -2（避免 CI 挂死）。 */
int run_rtdbd(const std::vector<std::string>& args, std::string* err_out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(RTDBD_BIN));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execv(RTDBD_BIN, argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    std::string err;
    char buf[4096];

    /* 置非阻塞，避免子进程写满管道而阻塞；配合 WNOHANG 轮询实现超时保护。 */
    int fl = fcntl(pipefd[0], F_GETFL, 0);
    if (fl >= 0) fcntl(pipefd[0], F_SETFL, fl | O_NONBLOCK);

    for (int i = 0; i < 250; ++i) {   /* 250 × 20ms = 5s 上限 */
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) err.append(buf, (size_t)n);

        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) err.append(buf, (size_t)n);
            close(pipefd[0]);
            if (err_out) *err_out = err;
            return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
        }
        usleep(20000);
    }

    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    close(pipefd[0]);
    if (err_out) *err_out = err;
    return -2;   /* 超时 */
}

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

/* 清理某实例的共享内存段。
 * B3 起 fail-fast 退出刻意**保留**段（不销毁数据），故测试须自行收尾，
 * 否则 /dev/shm 会随用例累积占用。
 * 注意：段若已存在，initialize 会 attach 成**非 owner**，此时 shutdown 不 unlink；
 * 故先 attach（解除原 owner 的遗留映射），再由 shm_unlink 直接删除段文件。 */
void drop_segment(const char* instance)
{
    if (indurtdb_initialize(instance, 64, 4) == 0) {
        char name[128];
        snprintf(name, sizeof(name), "/indurtdb_%s", instance);
        indurtdb_detach();          /* 解除映射且不 unlink */
        shm_unlink(name);           /* 显式删除段文件 */
    }
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

const char* kValidConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"Cfg_Valid\"\n"
    "    type: int32\n"
    "    access: 3\n"
    "    eur_min: 0\n"
    "    eur_max: 100\n"
    "    deadband: 1\n"
    "    flags: 3\n";

const char* kBadEurConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"Cfg_BadEur\"\n"
    "    type: int32\n"
    "    access: 3\n"
    "    eur_min: 100\n"
    "    eur_max: 0\n"
    "    flags: 1\n";

const char* kBadFlagsConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"Cfg_BadFlags\"\n"
    "    type: int32\n"
    "    access: 3\n"
    "    flags: 128\n";

const char* kBadPctConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"Cfg_BadPct\"\n"
    "    type: int32\n"
    "    access: 3\n"
    "    deadband: 10\n"
    "    flags: 6\n";

} // namespace

/* 1. 合法配置（含点位语义）正常启动，不进 fail-fast */
TEST(RtdbdConfig, ValidConfigStarts)
{
    std::string sock = "/tmp/indurtdb_cfg_valid.sock";
    std::string cfg  = make_tmp_file("cfg_valid.yaml", kValidConfig);
    unlink(sock.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd",
              "--socket", sock.c_str(),
              "--instance", "cfg_valid",
              "--config", cfg.c_str(),
              "--max-points", "64",
              "--max-subs", "4",
              (char*)NULL);
        _exit(127);
    }

    bool started = false;
    for (int i = 0; i < 300; ++i) {
        struct stat st;
        if (stat(sock.c_str(), &st) == 0) { started = true; break; }
        usleep(10000);
    }
    EXPECT_TRUE(started) << "valid config should start rtdbd";

    kill(pid, SIGTERM);
    int status = 0, reaped = 0;
    for (int i = 0; i < 500; ++i) {          /* 最多 5s，勿阻塞 CI */
        if (waitpid(pid, &status, WNOHANG) == pid) { reaped = 1; break; }
        usleep(10000);
    }
    if (!reaped) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
    unlink(sock.c_str());
}

/* 2. 配置文件不存在 → 退出码 2 */
TEST(RtdbdConfig, MissingConfigExits2)
{
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_missing.sock",
        "--instance", "cfg_missing",
        "--config", "/tmp/indurtdb_cfg_does_not_exist.yaml",
        "--max-points", "64",
        "--max-subs", "4"
    }, &err);
    EXPECT_EQ(code, 2) << err;
    EXPECT_NE(err.find("invalid config"), std::string::npos) << err;
    drop_segment("cfg_missing");
}

/* 3. EUR 上下限颠倒 → 退出码 2，打印点位与原因 */
TEST(RtdbdConfig, BadEurRangeExits2)
{
    std::string cfg = make_tmp_file("cfg_badeur.yaml", kBadEurConfig);
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_badeur.sock",
        "--instance", "cfg_badeur",
        "--config", cfg.c_str(),
        "--max-points", "64",
        "--max-subs", "4"
    }, &err);
    EXPECT_EQ(code, 2) << err;
    EXPECT_NE(err.find("point 10"), std::string::npos) << err;
    EXPECT_NE(err.find("eur_min"), std::string::npos) << err;
    drop_segment("cfg_badeur");
}

/* 4. meta flags 含未知位 → 退出码 2 */
TEST(RtdbdConfig, UnknownFlagsExits2)
{
    std::string err;
    std::string cfg = make_tmp_file("cfg_badflags.yaml", kBadFlagsConfig);
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_badflags.sock",
        "--instance", "cfg_badflags",
        "--config", cfg.c_str(),
        "--max-points", "64",
        "--max-subs", "4"
    }, &err);
    EXPECT_EQ(code, 2) << err;
    EXPECT_NE(err.find("flags"), std::string::npos) << err;
    drop_segment("cfg_badflags");
}

/* 6. 百分比死区但量程跨度无效 → 仍正常启动（仅告警），不进 fail-fast。
 *    回归护栏：曾一度把「百分比死区必须同时置 EUR 位」做成致命校验，
 *    结果拒绝了合法配置（主题A 的 NotifyDeadbandPercent 用例因此失败）。
 *    两者正交，故只告警。
 *    进程会常驻，故自行 fork + 探测 sock 出现后 SIGTERM 收尾。 */
TEST(RtdbdConfig, PercentWithoutRangeStillStarts)
{
    std::string cfg  = make_tmp_file("cfg_pctwarn.yaml", kBadPctConfig);
    std::string sock = "/tmp/indurtdb_cfg_pctwarn.sock";
    unlink(sock.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd",
              "--socket", sock.c_str(),
              "--instance", "cfg_pctwarn",
              "--config", cfg.c_str(),
              "--max-points", "64",
              "--max-subs", "4",
              (char*)NULL);
        _exit(127);
    }

    bool started = false;
    for (int i = 0; i < 300; ++i) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            /* 提前退出 = 被 fail-fast 拒绝，本用例失败 */
            int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
            EXPECT_NE(code, 2) << "must not fail-fast on percent-without-range";
            ADD_FAILURE() << "rtdbd exited early with code " << code;
            unlink(sock.c_str());
            return;
        }
        struct stat sb;
        if (stat(sock.c_str(), &sb) == 0) { started = true; break; }
        usleep(10000);
    }
    EXPECT_TRUE(started) << "percent-without-range config should still start rtdbd";

    kill(pid, SIGTERM);
    int st = 0;
    for (int i = 0; i < 500; ++i) {
        if (waitpid(pid, &st, WNOHANG) == pid) { pid = -1; break; }
        usleep(10000);
    }
    if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, &st, 0); }
    unlink(sock.c_str());
}

/* 7. 运行期 SET_META 写入非法 meta → 被拒（BAD_REQUEST），不落盘。
 *    护栏：若写路径不校验，非法 meta 会留存在段里，导致下次启动 fail-fast。 */
TEST(RtdbdConfig, SetMetaRejectsInvalid)
{
    std::string sock = "/tmp/indurtdb_cfg_setmeta.sock";
    std::string cfg  = make_tmp_file("cfg_setmeta.yaml", kValidConfig);
    unlink(sock.c_str());

    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd",
              "--socket", sock.c_str(),
              "--instance", "cfg_setmeta",
              "--config", cfg.c_str(),
              "--max-points", "64",
              "--max-subs", "4",
              (char*)NULL);
        _exit(127);
    }

    int fd = -1;
    for (int i = 0; i < 300; ++i) {
        fd = connect_to(sock);
        if (fd >= 0) break;
        usleep(10000);
    }
    ASSERT_GE(fd, 0);

    char pol[128];
    snprintf(pol, sizeof(pol), "%lu:0:63\n", (unsigned long)getuid());
    std::string pol_path = make_tmp_file("cfg_setmeta_pol.txt", pol);

    /* 重启一次让新实例加载 policy（策略在启动时读取） */
    kill(pid, SIGTERM);
    { int st=0; for (int i=0;i<500;++i){ if (waitpid(pid,&st,WNOHANG)==pid){pid=-1;break;} usleep(10000);} }
    if (pid > 0) { kill(pid, SIGKILL); int st=0; waitpid(pid,&st,0); }
    unlink(sock.c_str());

    pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd",
              "--socket", sock.c_str(),
              "--instance", "cfg_setmeta",
              "--policy", pol_path.c_str(),
              "--config", cfg.c_str(),
              "--max-points", "64",
              "--max-subs", "4",
              (char*)NULL);
        _exit(127);
    }
    for (int i = 0; i < 300; ++i) { if (connect_to(sock) >= 0) break; usleep(10000); }
    fd = connect_to(sock);
    ASSERT_GE(fd, 0);

    /* 非法 meta：EUR 开启但上下限颠倒 */
    rtdbd_set_meta_req_t s;
    memset(&s, 0, sizeof(s));
    s.point_id = 10;
    s.meta.flags   = 1;      /* INDURTDB_META_FLAG_EUR */
    s.meta.eur_min = 100.0;
    s.meta.eur_max = 0.0;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SET_META, &s, sizeof(s), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_BAD_REQUEST)
        << "SET_META with invalid meta must be rejected at write time";

    close(fd);
    kill(pid, SIGTERM);
    { int st=0; for (int i=0;i<500;++i){ if (waitpid(pid,&st,WNOHANG)==pid){pid=-1;break;} usleep(10000);} }
    if (pid > 0) { kill(pid, SIGKILL); int st=0; waitpid(pid,&st,0); }
    unlink(sock.c_str());
}

/* 8. 配置校验失败退出（exit 2）后，共享内存段仍存在（不得被 unlink 销毁）。
 *    护栏：曾用 indurtdb_shutdown() 退出，owner 语义会 shm_unlink 整个段，
 *    把「配置写错」变成「数据销毁」。 */
TEST(RtdbdConfig, Exit2PreservesSegment)
{
    std::string cfg  = make_tmp_file("cfg_preserve.yaml", kBadEurConfig);
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_preserve.sock",
        "--instance", "cfg_preserve",
        "--config", cfg.c_str(),
        "--max-points", "64",
        "--max-subs", "4"
    }, &err);
    ASSERT_EQ(code, 2) << err;

    /* 段应仍在：attach 后能读到 magic */
    EXPECT_EQ(indurtdb_initialize("cfg_preserve", 64, 4), 0)
        << "segment must survive a fail-fast exit";
    EXPECT_EQ(indurtdb_self_check(), INDURTDB_HEALTH_OK);
    indurtdb_detach();
    drop_segment("cfg_preserve");   /* 本用例验证段存活，验完自行清理 */
}

/* 9. 非法 --instance 字符 → 退出码 1 */
TEST(RtdbdConfig, BadInstanceExits1)
{
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_badinst.sock",
        "--instance", "bad/instance",
        "--max-points", "64",
        "--max-subs", "4"
    }, &err);
    EXPECT_EQ(code, 1) << err;
    EXPECT_NE(err.find("invalid --instance"), std::string::npos) << err;
}

/* 7. --max-points 0 → 退出码 1 */
TEST(RtdbdConfig, ZeroMaxPointsExits1)
{
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_zeropt.sock",
        "--instance", "cfg_zeropt",
        "--max-points", "0",
        "--max-subs", "4"
    }, &err);
    EXPECT_EQ(code, 1) << err;
    EXPECT_NE(err.find("invalid --max-points"), std::string::npos) << err;
}

/* 8. --max-subs 超上限 → 退出码 1 */
TEST(RtdbdConfig, OversizeMaxSubsExits1)
{
    std::string err;
    int code = run_rtdbd({
        "--socket", "/tmp/indurtdb_cfg_bigsub.sock",
        "--instance", "cfg_bigsub",
        "--max-points", "64",
        "--max-subs", "9999"
    }, &err);
    EXPECT_EQ(code, 1) << err;
    EXPECT_NE(err.find("invalid --max-subs"), std::string::npos) << err;
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
