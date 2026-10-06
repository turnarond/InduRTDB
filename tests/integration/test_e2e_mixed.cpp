/**
 * @file test_e2e_mixed.cpp
 * @brief v3.3 T6 端到端：host 多进程混合角色（驱动 / 控制逻辑 / HMI）
 *
 * 场景（对应 10-v3.3任务规划.md T6「红」）：
 *   - 驱动进程   ：容器/采集侧，经 indurtdb-client 写入（带 source_timestamp_ns）
 *   - 控制逻辑   ：本进程，经核心库直读共享内存
 *   - HMI 进程   ：经 rtdbd 订阅，跨进程接收变更通知
 *
 * 覆盖组合：
 *   1. DriverWriteVisibleToControl      —— 客户端写 → 控制面直读可见
 *   2. HmiReceivesNotifyWithSourceTs    —— 通知携带采集时刻（T4 + T5 联动）
 *   3. ReplayAfterRestartKeepsSourceTs  —— 服务重启后重放，值/时刻/顺序不失真（T2 + T3 + T5）
 *   4. MixedRolesNoFdLeak               —— 混合角色反复启停后无 fd 泄漏
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
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

struct Proc {
    std::string sock;
    std::string instance;
    std::string policy;
    std::string config;
    pid_t       pid = -1;

    bool start(const std::string& tag, const std::string& policy_body,
               const std::string& config_body = "")
    {
        sock     = "/tmp/indurtdb_e2e_" + tag + ".sock";
        instance = "e2e_test_" + tag;
        policy   = "/tmp/e2e_policy_" + tag + ".txt";
        config   = "/tmp/e2e_config_" + tag + ".yaml";

        FILE* fp = fopen(policy.c_str(), "w");
        if (fp) { fwrite(policy_body.data(), 1, policy_body.size(), fp); fclose(fp); }
        if (!config_body.empty()) {
            FILE* fc = fopen(config.c_str(), "w");
            if (fc) { fwrite(config_body.data(), 1, config_body.size(), fc); fclose(fc); }
        }

        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            /* 守护进程不得继承测试进程的 stdio：
             * 否则父进程异常退出后成为孤儿，仍持有输出管道，
             * 会挂住 ctest / CI 的输出采集（T6 实测踩到）。
             * 同时 setsid 脱离进程组，避免与测试进程互相信号干扰。 */
            setsid();
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO) close(devnull);
            }
            if (config_body.empty()) {
                execl(RTDBD_BIN, "rtdbd",
                      "--socket", sock.c_str(),
                      "--instance", instance.c_str(),
                      "--policy", policy.c_str(),
                      "--max-points", "64",
                      "--max-subs", "8",
                      (char*)NULL);
            } else {
                execl(RTDBD_BIN, "rtdbd",
                      "--socket", sock.c_str(),
                      "--instance", instance.c_str(),
                      "--policy", policy.c_str(),
                      "--config", config.c_str(),
                      "--max-points", "64",
                      "--max-subs", "8",
                      (char*)NULL);
            }
            _exit(127);
        }
        return wait_socket(500);
    }

    bool wait_socket(int tries)
    {
        for (int i = 0; i < tries; ++i) {
            struct stat st;
            if (stat(sock.c_str(), &st) == 0) return true;
            usleep(10000);
        }
        return false;
    }

    void kill_hard()
    {
        if (pid > 0) {
            kill(pid, SIGKILL);
            int status = 0, reaped = 0;
            for (int i = 0; i < 500; ++i) {          /* 最多 5s，勿阻塞 CI */
                if (waitpid(pid, &status, WNOHANG) == pid) { reaped = 1; break; }
                usleep(10000);
            }
            if (!reaped) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
            pid = -1;
        }
    }

    void stop()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0, reaped = 0;
            for (int i = 0; i < 500; ++i) {          /* 最多 5s，勿阻塞 CI */
                if (waitpid(pid, &status, WNOHANG) == pid) { reaped = 1; break; }
                usleep(10000);
            }
            if (!reaped) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
            pid = -1;
        }
        unlink(sock.c_str());
        unlink(policy.c_str());
    }

    ~Proc() { stop(); }
};

int connect_to(const std::string& sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    for (int i = 0; i < 300; ++i) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        usleep(10000);
    }
    close(fd);
    return -1;
}

bool send_req(int fd, uint16_t opcode, const void* payload, uint32_t len)
{
    rtdbd_req_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.magic = RTDBD_MAGIC;
    h.version = RTDBD_PROTO_VERSION;
    h.opcode = opcode;
    h.payload_len = len;
    if (send(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) return false;
    if (len > 0 && payload && send(fd, payload, len, 0) != (ssize_t)len) return false;
    return true;
}

bool subscribe(int fd, uint32_t point_id)
{
    rtdbd_sub_req_t s;
    memset(&s, 0, sizeof(s));
    s.point_id = point_id;
    if (!send_req(fd, RTDBD_OP_SUBSCRIBE, &s, sizeof(s))) return false;
    rtdbd_resp_hdr_t r;
    if (recv(fd, &r, sizeof(r), MSG_WAITALL) != (ssize_t)sizeof(r)) return false;
    return r.status == RTDBD_ST_OK;
}

std::string policy_for_self()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

int count_fds(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    DIR* d = opendir(path);
    if (!d) return -1;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        n++;
    }
    closedir(d);
    return n;
}

/* ---- 驱动进程（子进程）：经客户端同步写入，携带采集时刻 ---- */
bool driver_write_int32(const std::string& sock, uint32_t id, int32_t value,
                        uint64_t source_ts_ns)
{
    irtcli_t c;
    if (irtcli_init(&c, sock.c_str(), 0, NULL, NULL) != IRTCLI_OK) return false;
    irtcli_set_async(&c, false);
    if (irtcli_connect(&c) != IRTCLI_OK) { irtcli_close(&c); return false; }
    int rc = irtcli_write_int32(&c, id, value, source_ts_ns);
    irtcli_close(&c);
    return rc == IRTCLI_OK;
}

bool run_driver_child(const std::string& sock, uint32_t id, int32_t value,
                      uint64_t source_ts_ns)
{
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) _exit(driver_write_int32(sock, id, value, source_ts_ns) ? 0 : 1);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* ---- HMI 进程（子进程）：订阅并接收一次通知 ---- */
struct NotifyResult {
    uint32_t point_id;
    int32_t  value;
    uint64_t source_ts_ns;
};

/* HMI 子进程先启动并订阅，就绪后通知父进程，父进程再触发写入 */
struct HmiChild {
    pid_t pid = -1;
    int   rfd = -1;
};

bool hmi_spawn(const std::string& sock, uint32_t id, HmiChild* out)
{
    int pfd[2];
    if (pipe(pfd) < 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(pfd[0]); close(pfd[1]); return false; }
    if (pid == 0) {
        close(pfd[0]);
        int fd = connect_to(sock);
        if (fd < 0 || !subscribe(fd, id)) { close(pfd[1]); _exit(2); }

        /* 就绪：订阅已生效，父进程可以触发写入了 */
        char ready = 'R';
        ssize_t w = write(pfd[1], &ready, 1);
        (void)w;

        struct pollfd p;
        p.fd = fd; p.events = POLLIN;
        if (poll(&p, 1, 2000) <= 0) { close(fd); close(pfd[1]); _exit(3); }

        rtdbd_req_hdr_t nh;
        rtdbd_notify_t  nt;
        bool ok = recv(fd, &nh, sizeof(nh), MSG_WAITALL) == (ssize_t)sizeof(nh) &&
                  nh.opcode == RTDBD_OP_NOTIFY &&
                  recv(fd, &nt, sizeof(nt), MSG_WAITALL) == (ssize_t)sizeof(nt) &&
                  nt.point_id == id;
        if (ok) {
            NotifyResult r;
            r.point_id      = nt.point_id;
            r.value         = (int32_t)(int64_t)nt.value_bits;
            r.source_ts_ns  = nt.source_ts_ns;
            ssize_t w = write(pfd[1], &r, sizeof(r));
            (void)w;
        }
        close(fd);
        close(pfd[1]);
        _exit(ok ? 0 : 4);
    }

    close(pfd[1]);
    out->pid = pid;
    out->rfd = pfd[0];
    return true;
}

bool hmi_join(HmiChild* h, NotifyResult* out)
{
    ssize_t n = read(h->rfd, out, sizeof(*out));
    close(h->rfd);
    h->rfd = -1;
    int status = 0;
    waitpid(h->pid, &status, 0);
    h->pid = -1;
    bool exited_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return exited_ok && n == (ssize_t)sizeof(*out);
}

/* 等待 HMI 订阅就绪 */
bool hmi_wait_ready(const HmiChild* h)
{
    char ready = 0;
    return read(h->rfd, &ready, 1) == 1 && ready == 'R';
}

} // namespace

/* 1. 驱动（客户端）写入 → 控制逻辑（核心库直读 shm）可见 */
TEST(E2E, DriverWriteVisibleToControl)
{
    Proc p;
    ASSERT_TRUE(p.start("drv", policy_for_self()));

    /* 控制面先 attach 共享内存（非 owner，只读） */
    ASSERT_EQ(indurtdb_initialize(p.instance.c_str(), 64, 8), 0);

    ASSERT_TRUE(run_driver_child(p.sock, 7, 42, 0)) << "驱动进程写入应成功";
    int32_t v = 0;
    EXPECT_EQ(indurtdb_read_int32(7, &v), 0);
    EXPECT_EQ(v, 42) << "控制逻辑应直读到驱动写入的值";
    indurtdb_shutdown();
}

/* 2. HMI 收到的通知携带驱动给出的采集时刻（T4 + T5 联动） */
TEST(E2E, HmiReceivesNotifyWithSourceTs)
{
    Proc p;
    ASSERT_TRUE(p.start("hmi", policy_for_self()));

    HmiChild hmi;
    ASSERT_TRUE(hmi_spawn(p.sock, 8, &hmi)) << "HMI 进程应能启动";
    ASSERT_TRUE(hmi_wait_ready(&hmi)) << "HMI 订阅应就绪";

    const uint64_t src_ts = 1234567890ULL;
    ASSERT_TRUE(run_driver_child(p.sock, 8, 77, src_ts)) << "驱动写入应携带采集时刻";

    NotifyResult r;
    memset(&r, 0, sizeof(r));
    ASSERT_TRUE(hmi_join(&hmi, &r)) << "HMI 进程应收到变更通知";
    EXPECT_EQ(r.point_id, 8u);
    EXPECT_EQ(r.value, 77);
    EXPECT_EQ(r.source_ts_ns, src_ts) << "通知中的采集时刻不得被改写为入库时刻";
}

/* 3. 服务被杀 → 客户端异步入队 → 重启后重放：值 / 采集时刻均不失真 */
TEST(E2E, ReplayAfterRestartKeepsSourceTs)
{
    Proc p;
    ASSERT_TRUE(p.start("replay", policy_for_self()));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, NULL, NULL), IRTCLI_OK);
    irtcli_set_async(&c, true);
    ASSERT_EQ(irtcli_connect(&c), IRTCLI_OK);

    /* 服务不可用期间：写必须入队，绝不阻塞、绝不丢 */
    p.kill_hard();
    const uint64_t ts_base = 5000000000ULL;
    EXPECT_EQ(irtcli_write_int32(&c, 3, 10, ts_base + 1), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 4, 20, ts_base + 2), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_write_int32(&c, 5, 30, ts_base + 3), IRTCLI_QUEUED);
    EXPECT_EQ(irtcli_queue_count(&c), 3u);

    /* 重启并 attach 已有段，再重放 */
    ASSERT_TRUE(p.start("replay", policy_for_self()));
    int flushed = irtcli_flush(&c);
    EXPECT_EQ(flushed, 3) << "队列中的 3 条应在恢复后全部提交";
    irtcli_close(&c);

    ASSERT_EQ(indurtdb_initialize(p.instance.c_str(), 64, 8), 0);
    const uint32_t ids[3]   = {3, 4, 5};
    const int32_t  vals[3]  = {10, 20, 30};
    for (int i = 0; i < 3; ++i) {
        indurtdb_point_t pt;
        ASSERT_EQ(indurtdb_read_point(ids[i], &pt), 0);
        EXPECT_EQ(pt.value.i, vals[i]) << "重放后点位 " << ids[i] << " 的值应正确";
        EXPECT_EQ(pt.source_timestamp_ns, ts_base + (uint64_t)(i + 1))
            << "重放必须携带原始采集时刻，不得改写";
    }
    indurtdb_shutdown();
}

/* 4. 混合角色反复连接后退订/断开，服务端不应累积 fd */
TEST(E2E, MixedRolesNoFdLeak)
{
    Proc p;
    ASSERT_TRUE(p.start("mixed", policy_for_self()));

    int before = count_fds(p.pid);
    ASSERT_GT(before, 0);

    for (int i = 0; i < 60; ++i) {
        int fd = connect_to(p.sock);
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(subscribe(fd, 9));
        int wfd = connect_to(p.sock);
        ASSERT_GE(wfd, 0);
        ASSERT_TRUE(run_driver_child(p.sock, 9, i, (uint64_t)i));
        close(fd);
        close(wfd);
    }
    usleep(300000);

    int after = count_fds(p.pid);
    printf("[FD] before=%d after=%d\n", before, after);
    EXPECT_LE(after, before + 8) << "混合角色反复启停不应累积 fd";
}

/* 5. 混合角色端到端扩展（v3.4 T10）：客户端经 daemon 按名查找 + 写/读元数据，
 *    控制面（核心库直读共享内存）能看到同一份元数据 —— 验证读写分离架构下
 *    "名称 → id → 元数据"全链路，且 daemon 是唯一元数据写者。 */
const char* kE2EConfig =
    "points:\n"
    "  - id: 10\n"
    "    name: \"AHU_01.Supply_Temp\"\n"
    "    type: int32\n"
    "    unit: 0\n"
    "    access: 3\n"
    "  - id: 11\n"
    "    name: \"Pump_Start_CMD\"\n"
    "    type: bool\n"
    "    access: 3\n";

TEST(E2E, FindByNameAndMetaThroughDaemon)
{
    Proc p;
    ASSERT_TRUE(p.start("fnm", policy_for_self(), kE2EConfig));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, NULL, NULL), IRTCLI_OK);
    irtcli_set_async(&c, false);
    ASSERT_EQ(irtcli_connect(&c), IRTCLI_OK);

    /* 按名查找（走 daemon 共享索引） */
    uint32_t id = 0;
    ASSERT_EQ(irtcli_find_by_name(&c, "AHU_01.Supply_Temp", &id), IRTCLI_OK);
    EXPECT_EQ(id, 10u);
    uint32_t missing = 0;
    EXPECT_EQ(irtcli_find_by_name(&c, "NoSuch.Point", &missing), IRTCLI_ERR_NOT_FOUND);

    /* 经 daemon 写元数据（管控写，需本机 uid 授权） */
    indurtdb_meta_t m;
    memset(&m, 0, sizeof(m));
    m.eur_min = -10.0;
    m.eur_max = 50.0;
    m.deadband = 0.5f;
    m.flags = 0x3u;
    ASSERT_EQ(irtcli_set_meta(&c, 10, &m), IRTCLI_OK);

    /* 经 daemon 读回 */
    indurtdb_meta_t r;
    memset(&r, 0, sizeof(r));
    ASSERT_EQ(irtcli_get_meta(&c, 10, &r), IRTCLI_OK);
    EXPECT_DOUBLE_EQ(r.eur_min, -10.0);
    EXPECT_DOUBLE_EQ(r.eur_max, 50.0);
    EXPECT_FLOAT_EQ(r.deadband, 0.5f);
    EXPECT_EQ(r.flags, 0x3u);
    irtcli_close(&c);

    /* 控制面直读共享内存：应看到 daemon 写入的同一份元数据（写权威语义） */
    ASSERT_EQ(indurtdb_initialize(p.instance.c_str(), 64, 8), 0);
    indurtdb_meta_t ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ASSERT_EQ(indurtdb_get_meta(10, &ctrl), 0);
    EXPECT_DOUBLE_EQ(ctrl.eur_min, -10.0);
    EXPECT_DOUBLE_EQ(ctrl.eur_max, 50.0);
    EXPECT_FLOAT_EQ(ctrl.deadband, 0.5f);
    EXPECT_EQ(ctrl.flags, 0x3u);
    indurtdb_shutdown();
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
