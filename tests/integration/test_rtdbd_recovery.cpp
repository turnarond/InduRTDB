/**
 * @file test_rtdbd_recovery.cpp
 * @brief rtdbd 快速自恢复（T2 红阶段）
 *
 * 覆盖：
 *   - RestartAttachesExistingSegment   SIGKILL 后重启，attach 已有段，数据不丢
 *   - ReclaimsOwnershipAfterRestart    重启后重新取得所有权，后续写正常
 *   - RtoWithinBudget                  恢复时间 ≤ 1s（设计目标 ≤100ms）
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>
#include <string>

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

struct SupProc {
    std::string sock;
    std::string instance;
    std::string policy;
    std::string pidfile;
    pid_t       pid = -1;

    bool start(const std::string& tag, const std::string& policy_body)
    {
        sock     = "/tmp/indurtdb_rec_" + tag + ".sock";
        instance = "rec_test_" + tag;
        policy   = "/tmp/rec_policy_" + tag + ".txt";
        pidfile  = "/tmp/rec_worker_" + tag + ".pid";

        FILE* f = fopen(policy.c_str(), "w");
        if (f) { fwrite(policy_body.data(), 1, policy_body.size(), f); fclose(f); }

        unlink(sock.c_str());
        unlink(pidfile.c_str());

        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy.c_str(),
                  "--max-points", "64",
                  "--max-subs", "4",
                  "--supervise",
                  "--pidfile", pidfile.c_str(),
                  (char*)NULL);
            _exit(127);
        }

        for (int i = 0; i < 500; ++i) {
            struct stat st;
            if (stat(sock.c_str(), &st) == 0 && worker_pid() > 0) return true;
            usleep(10000);
        }
        return false;
    }

    ~SupProc() { stop(); }

    void stop()
    {
        if (pid > 0) {
            /* 有界等待 + SIGKILL 兜底：绝不让 CI 挂死在 waitpid 上
             * （supervisor 若卡住，阻塞 waitpid 会把整个 ctest 拖死）。 */
            kill(pid, SIGTERM);
            int reaped = 0;
            for (int i = 0; i < 500; ++i) {          /* 最多 5s */
                int st = 0;
                if (waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
                usleep(10000);
            }
            if (!reaped) {
                kill(pid, SIGKILL);
                int st = 0;
                waitpid(pid, &st, 0);
            }
            pid = -1;
        }
        unlink(sock.c_str());
        unlink(pidfile.c_str());
    }

    int worker_pid()
    {
        FILE* f = fopen(pidfile.c_str(), "r");
        if (!f) return -1;
        int v = -1;
        if (fscanf(f, "%d", &v) != 1) v = -1;
        fclose(f);
        return v;
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
    for (int i = 0; i < 100; ++i) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) return fd;
        usleep(10000);
    }
    close(fd);
    return -1;
}

/* 经 rtdbd 写 int32，返回服务端状态码；连不上返回 -1 */
int write_via_rtdbd(const std::string& sock, uint32_t id, int32_t value, uint64_t src_ts = 0)
{
    int fd = connect_to(sock);
    if (fd < 0) return -1;

    rtdbd_req_hdr_t  req;
    rtdbd_write_req_t w;
    memset(&req, 0, sizeof(req));
    memset(&w, 0, sizeof(w));
    req.magic = RTDBD_MAGIC;
    req.version = RTDBD_PROTO_VERSION;
    req.opcode = RTDBD_OP_WRITE;
    req.payload_len = (uint32_t)sizeof(w);
    w.point_id = id;
    w.type = RTDBD_TYPE_INT32;
    w.value_bits = (uint64_t)(int64_t)value;
    w.source_ts_ns = src_ts;

    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) { close(fd); return -1; }
    if (send(fd, &w, sizeof(w), 0) != (ssize_t)sizeof(w)) { close(fd); return -1; }

    rtdbd_resp_hdr_t resp;
    if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) != (ssize_t)sizeof(resp)) { close(fd); return -1; }
    close(fd);
    return (int)resp.status;
}

bool shm_read_int32(const std::string& instance, uint32_t id, int32_t* out)
{
    if (indurtdb_initialize(instance.c_str(), 64, 4) != 0) return false;
    int32_t v = 0;
    bool ok = (indurtdb_read_int32(id, &v) == 0);
    if (ok && out) *out = v;
    indurtdb_shutdown();
    return ok;
}

std::string policy_for_self()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

uint64_t now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

} // namespace

/* 1. SIGKILL 后重启：attach 已有段，数据不丢 */
TEST(RtdbdRecovery, RestartAttachesExistingSegment)
{
    SupProc p;
    ASSERT_TRUE(p.start("attach", policy_for_self()));

    ASSERT_EQ(write_via_rtdbd(p.sock, 1, 42), (int)RTDBD_ST_OK);

    int before = p.worker_pid();
    ASSERT_GT(before, 0);
    ASSERT_EQ(kill(before, SIGKILL), 0);

    /* 等待新 worker 出现 */
    int after = -1;
    for (int i = 0; i < 500; ++i) {
        int cur = p.worker_pid();
        if (cur > 0 && cur != before) { after = cur; break; }
        usleep(10000);
    }
    ASSERT_GT(after, 0) << "supervisor 应自动拉起新 worker";

    int32_t v = 0;
    ASSERT_TRUE(shm_read_int32(p.instance, 1, &v));
    EXPECT_EQ(v, 42) << "SIGKILL 后段仍存在，重启应 attach 已有段而非重建，数据不丢";
}

/* 2. 重启后重新取得所有权，后续写正常 */
TEST(RtdbdRecovery, ReclaimsOwnershipAfterRestart)
{
    SupProc p;
    ASSERT_TRUE(p.start("reclaim", policy_for_self()));

    ASSERT_EQ(write_via_rtdbd(p.sock, 2, 10), (int)RTDBD_ST_OK);

    int before = p.worker_pid();
    ASSERT_EQ(kill(before, SIGKILL), 0);

    int after = -1;
    for (int i = 0; i < 500; ++i) {
        int cur = p.worker_pid();
        if (cur > 0 && cur != before) { after = cur; break; }
        usleep(10000);
    }
    ASSERT_GT(after, 0);

    /* 重启后写入必须成功 */
    EXPECT_EQ(write_via_rtdbd(p.sock, 2, 99), (int)RTDBD_ST_OK);

    int32_t v = 0;
    ASSERT_TRUE(shm_read_int32(p.instance, 2, &v));
    EXPECT_EQ(v, 99);
}

/* 若某实例的共享内存段仍存在则删除（用例收尾，避免 /dev/shm 累积） */
void drop_segment_if_exists(const std::string& instance)
{
    char name[128];
    snprintf(name, sizeof(name), "/indurtdb_%s", instance.c_str());
    if (shm_unlink(name) != 0) return;   /* 本就不存在：忽略 */
}

/* 3. supervisor 退出后不得留下 worker（孤儿护栏）。
 *    评审发现：worker 可能阻塞在 recv_all() 中，而 recv_all 的 EINTR 分支是
 *    continue 重试 recv、不检查 g_stop —— 只发 SIGTERM 便退出的 supervisor
 *    会留下永不退出的孤儿（占 shm 段/socket，且继承的 stdout/stderr 管道写端
 *    不关闭，会把等 EOF 的调用方永久挂住）。本用例钉死"两个 PID 都消失"。 */
static bool pid_alive(int pid, bool* unknown = nullptr)
{
    if (pid <= 0) return false;
    if (kill((pid_t)pid, 0) != 0) return false;   /* 已退出 */
    /* 存活：排除僵尸（已终止但未回收） */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE* f = fopen(path, "r");
    if (!f) { if (unknown) *unknown = true; return true; }
    char state = '?';
    if (fscanf(f, "%*d %*s %c", &state) != 1) state = '?';
    fclose(f);
    return (state != 'Z');
}

TEST(RtdbdRecovery, NoOrphanWorkerAfterStop)
{
    SupProc p;
    ASSERT_TRUE(p.start("orphan", policy_for_self()));

    int worker = p.worker_pid();
    ASSERT_GT(worker, 0);

    /* 连一个客户端并保持半开请求，让 worker 有机会停在 recv_all() 内 */
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    rtdbd_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic       = RTDBD_MAGIC;
    req.version     = RTDBD_PROTO_VERSION;
    req.opcode      = RTDBD_OP_WRITE;
    req.payload_len = (uint32_t)sizeof(rtdbd_write_req_t);
    (void)send(fd, &req, sizeof(req), 0);   /* 只发头，不发负载 → 服务端停在 recv_all */

    usleep(50000);   /* 让 worker 进入 recv_all */

    int sup = p.pid;
    ASSERT_GT(sup, 0);

    p.stop();   /* SIGTERM supervisor → 必须连带回收 worker */

    /* 两者都必须消失（留 1s 观察窗） */
    bool sup_gone = false, wk_gone = false;
    for (int i = 0; i < 100; ++i) {
        if (!pid_alive(sup))    sup_gone = true;
        if (!pid_alive(worker)) wk_gone = true;
        if (sup_gone && wk_gone) break;
        usleep(10000);
    }
    EXPECT_TRUE(sup_gone)  << "supervisor should exit on SIGTERM";
    EXPECT_TRUE(wk_gone)   << "worker must not be left as an orphan "
                              "(holds shm segment, socket, and inherited stdout pipe)";

    close(fd);
    /* 段也应随 worker 退出而释放（owner 关闭 unlink） */
    drop_segment_if_exists(p.instance);
}

/* 4. 恢复时间预算：≤ 1s（设计目标 ≤100ms） */
TEST(RtdbdRecovery, RtoWithinBudget)
{
    SupProc p;
    ASSERT_TRUE(p.start("rto", policy_for_self()));

    int before = p.worker_pid();
    ASSERT_GT(before, 0);

    uint64_t t0 = now_ms();
    ASSERT_EQ(kill(before, SIGKILL), 0);

    /* 以"能再次成功写入"作为恢复完成标志 */
    bool recovered = false;
    for (int i = 0; i < 1000; ++i) {
        if (write_via_rtdbd(p.sock, 3, 7) == (int)RTDBD_ST_OK) { recovered = true; break; }
        usleep(1000);
    }
    uint64_t elapsed = now_ms() - t0;

    ASSERT_TRUE(recovered) << "worker 被杀后应自动恢复并可写入";
    printf("[RTO] measured = %llu ms\n", (unsigned long long)elapsed);
    EXPECT_LE(elapsed, 1000u) << "恢复时间应 ≤ 1s（现场确认可接受上限；设计目标 ≤100ms）";
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
