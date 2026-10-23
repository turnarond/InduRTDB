/**
 * @file test_rtdbd_notify.cpp
 * @brief rtdbd 变更通知（T4 红阶段）
 *
 * 覆盖：
 *   - CrossProcessReceivesChange  跨进程订阅者收到变更通知
 *   - NoFdLeakUnderChurn          大量订阅/退订循环后不泄漏 fd
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
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

struct Proc {
    std::string sock;
    std::string instance;
    std::string policy;
    pid_t       pid = -1;

    bool start(const std::string& tag, const std::string& policy_body)
    {
        return start_with(tag, policy_body, 64, 4);
    }

    bool start_with(const std::string& tag, const std::string& policy_body,
                    uint32_t max_points, uint32_t max_subs)
    {
        sock     = "/tmp/indurtdb_notify_" + tag + ".sock";
        instance = "notify_test_" + tag;
        shm_unlink(("/indurtdb_" + instance).c_str());
        policy   = "/tmp/notify_policy_" + tag + ".txt";

        FILE* f = fopen(policy.c_str(), "w");
        if (f) { fwrite(policy_body.data(), 1, policy_body.size(), f); fclose(f); }

        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            const std::string mp = std::to_string(max_points);
            const std::string ms = std::to_string(max_subs);
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy.c_str(),
                  "--max-points", mp.c_str(),
                  "--max-subs", ms.c_str(),
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

    /* 带 YAML 点位配置启动：用于构造“已注册但策略拒绝”的点位。 */
    bool start_cfg(const std::string& tag, const std::string& policy_body,
                   uint32_t max_points, uint32_t max_subs,
                   const std::string& cfg_body)
    {
        sock     = "/tmp/indurtdb_notify_" + tag + ".sock";
        instance = "notify_test_" + tag;
        shm_unlink(("/indurtdb_" + instance).c_str());
        policy   = "/tmp/notify_policy_" + tag + ".txt";
        std::string cfg = "/tmp/notify_cfg_" + tag + ".yaml";

        FILE* f = fopen(policy.c_str(), "w");
        if (f) { fwrite(policy_body.data(), 1, policy_body.size(), f); fclose(f); }
        f = fopen(cfg.c_str(), "w");
        if (f) { fwrite(cfg_body.data(), 1, cfg_body.size(), f); fclose(f); }

        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            const std::string mp = std::to_string(max_points);
            const std::string ms = std::to_string(max_subs);
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy.c_str(),
                  "--config", cfg.c_str(),
                  "--max-points", mp.c_str(),
                  "--max-subs", ms.c_str(),
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

    ~Proc() { stop(); }

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
    memset(&h, 0, sizeof(h));
    h.magic = RTDBD_MAGIC;
    h.version = RTDBD_PROTO_VERSION;
    h.opcode = opcode;
    h.payload_len = len;
    if (send(fd, &h, sizeof(h), 0) != (ssize_t)sizeof(h)) return false;
    if (len > 0 && payload && send(fd, payload, len, 0) != (ssize_t)len) return false;
    return true;
}

bool recv_resp(int fd, rtdbd_resp_hdr_t* out)
{
    return recv(fd, out, sizeof(*out), MSG_WAITALL) == (ssize_t)sizeof(*out);
}

bool subscribe(int fd, uint32_t point_id)
{
    rtdbd_sub_req_t s;
    memset(&s, 0, sizeof(s));
    s.point_id = point_id;
    if (!send_req(fd, RTDBD_OP_SUBSCRIBE, &s, sizeof(s))) return false;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return false;
    return r.status == RTDBD_ST_OK;
}

bool unsubscribe(int fd, uint32_t point_id)
{
    rtdbd_sub_req_t s;
    memset(&s, 0, sizeof(s));
    s.point_id = point_id;
    if (!send_req(fd, RTDBD_OP_UNSUBSCRIBE, &s, sizeof(s))) return false;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return false;
    return r.status == RTDBD_ST_OK;
}

/* SET_SUBS 批量订阅：发送 count + ids[]，返回响应状态码 */
uint16_t set_subs(int fd, const std::vector<uint32_t>& ids)
{
    std::vector<uint8_t> payload(4 + 4 * ids.size());
    uint32_t count = (uint32_t)ids.size();
    memcpy(payload.data(), &count, 4);
    for (size_t i = 0; i < ids.size(); ++i) {
        memcpy(payload.data() + 4 + 4 * i, &ids[i], 4);
    }
    if (!send_req(fd, RTDBD_OP_SET_SUBS, payload.data(), (uint32_t)payload.size())) return 0xFFFF;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return 0xFFFF;
    return r.status;
}

/* 接收一帧 NOTIFY（含头部），超时 ms 返回 false */
bool recv_notify(int fd, rtdbd_notify_t* out, int timeout_ms = 2000)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready <= 0) return false;

    rtdbd_req_hdr_t nh;
    if (recv(fd, &nh, sizeof(nh), MSG_WAITALL) != (ssize_t)sizeof(nh)) return false;
    if (nh.magic != RTDBD_MAGIC || nh.opcode != RTDBD_OP_NOTIFY) return false;
    if (recv(fd, out, sizeof(*out), MSG_WAITALL) != (ssize_t)sizeof(*out)) return false;
    return true;
}

bool recv_payload(int fd, void* out, uint32_t len)
{
    return recv(fd, out, len, MSG_WAITALL) == (ssize_t)len;
}

/* OP_CAPS：读 32B 能力结构；成功要求状态 OK 且负载长度精确匹配。 */
bool caps_exchange(int fd, rtdbd_caps_t* out)
{
    if (!send_req(fd, RTDBD_OP_CAPS, NULL, 0)) return false;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return false;
    if (r.status != RTDBD_ST_OK || r.payload_len != sizeof(*out)) return false;
    return recv_payload(fd, out, sizeof(*out));
}

/* 接收一帧 SNAPSHOT_DONE（服务端→客户端控制帧）。 */
bool recv_done(int fd, rtdbd_snapshot_done_t* out, int timeout_ms = 5000)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, timeout_ms) <= 0) return false;

    rtdbd_req_hdr_t h;
    if (recv(fd, &h, sizeof(h), MSG_WAITALL) != (ssize_t)sizeof(h)) return false;
    if (h.magic != RTDBD_MAGIC || h.opcode != RTDBD_OP_SNAPSHOT_DONE) return false;
    if (h.payload_len != sizeof(*out)) return false;
    return recv_payload(fd, out, sizeof(*out));
}

std::string yaml_points(uint32_t n)
{
    std::string out = "points:\n";
    char line[160];
    for (uint32_t i = 0; i < n; ++i) {
        snprintf(line, sizeof(line),
                 "  - id: %u\n"
                 "    name: \"P%u\"\n"
                 "    type: int32\n"
                 "    access: 3\n",
                 i, i);
        out += line;
    }
    return out;
}

bool write_int32(int fd, uint32_t point_id, int32_t value)
{
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id = point_id;
    w.type = RTDBD_TYPE_INT32;
    w.value_bits = (uint64_t)(int64_t)value;
    if (!send_req(fd, RTDBD_OP_WRITE, &w, sizeof(w))) return false;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return false;
    return r.status == RTDBD_ST_OK;
}

bool create_point(int fd, uint32_t point_id, const char* name)
{
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id  = point_id;
    c.type      = RTDBD_TYPE_INT32;
    c.access    = 3;
    c.partition = 0;
    snprintf(c.name, sizeof(c.name), "%s", name);
    if (!send_req(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c))) return false;
    rtdbd_resp_hdr_t r;
    if (!recv_resp(fd, &r)) return false;
    return r.status == RTDBD_ST_OK;
}

std::string policy_for_self_range(uint32_t last_id)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:*:0:%u\n", (unsigned long)getuid(), last_id);
    return std::string(buf);
}

std::string policy_for_self()
{
    return policy_for_self_range(63);
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

} // namespace

/* 1. 跨进程订阅者收到变更通知 */
TEST(RtdbdNotify, CrossProcessReceivesChange)
{
    Proc p;
    ASSERT_TRUE(p.start("chg", policy_for_self()));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    int wri = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);
    ASSERT_GE(wri, 0);

    char name[32];
    snprintf(name, sizeof(name), "Chg_5");
    ASSERT_TRUE(create_point(ctl, 5, name)) << "create point 5";

    ASSERT_TRUE(subscribe(sub, 5)) << "订阅应成功";

    ASSERT_TRUE(write_int32(wri, 5, 123));

    /* 订阅者应收到服务端推送的变更通知 */
    struct pollfd pfd;
    pfd.fd = sub;
    pfd.events = POLLIN;
    int ready = poll(&pfd, 1, 2000);
    ASSERT_GT(ready, 0) << "写入后订阅者应收到通知（此前订阅仅进程内生效）";

    rtdbd_req_hdr_t nh;
    ASSERT_TRUE(recv(sub, &nh, sizeof(nh), MSG_WAITALL) == (ssize_t)sizeof(nh));
    EXPECT_EQ(nh.magic, RTDBD_MAGIC);
    EXPECT_EQ(nh.opcode, RTDBD_OP_NOTIFY);

    rtdbd_notify_t nt;
    ASSERT_TRUE(recv(sub, &nt, sizeof(nt), MSG_WAITALL) == (ssize_t)sizeof(nt));
    EXPECT_EQ(nt.point_id, 5u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 123);

    close(sub);
    close(wri);
    close(ctl);
}

/* 2. 大量订阅/退订循环后不泄漏 fd */
TEST(RtdbdNotify, NoFdLeakUnderChurn)
{
    Proc p;
    ASSERT_TRUE(p.start("churn", policy_for_self()));

    int ctl = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    char name[32];
    snprintf(name, sizeof(name), "Churn_6");
    ASSERT_TRUE(create_point(ctl, 6, name)) << "create point 6";
    close(ctl);

    int before = count_fds(p.pid);
    ASSERT_GT(before, 0);

    for (int i = 0; i < 100; ++i) {
        int fd = connect_to(p.sock);
        ASSERT_GE(fd, 0);
        ASSERT_TRUE(subscribe(fd, 6));
        ASSERT_TRUE(unsubscribe(fd, 6));
        close(fd);
    }

    /* 给服务端一点时间回收 */
    usleep(200000);

    int after = count_fds(p.pid);
    ASSERT_GT(after, 0);
    printf("[FD] before=%d after=%d\n", before, after);
    EXPECT_LE(after, before + 8) << "反复订阅/退订不应累积 fd";
}

/* 3. 每连接最多 1024 个订阅（v3.10 容量提升；v3.11 默认上限为 10000） */
TEST(RtdbdNotify, SubscribeUpTo1024)
{
    Proc p;
    ASSERT_TRUE(p.start_with("sub1024", policy_for_self_range(1023),
                             1024, 1024));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 0; i < 1024; ++i) {
        snprintf(name, sizeof(name), "Scale_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
    }

    for (uint32_t i = 0; i < 1024; ++i) {
        ASSERT_TRUE(subscribe(sub, i)) << "subscribe " << i;
    }

    close(sub);
    close(ctl);
}

/* 4. --max-subs 按连接生效：--max-subs 4 时第 5 个订阅被拒 */
TEST(RtdbdNotify, MaxSubsPerConnectionEnforced)
{
    Proc p;
    ASSERT_TRUE(p.start_with("maxsubs4", policy_for_self(), 64, 4));

    int ctl = connect_to(p.sock);
    int fd  = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(fd, 0);

    /* v3.10 T4 起 SUBSCRIBE 会校验“已注册”，故先注册 0..4。 */
    char name[32];
    for (uint32_t i = 0; i < 5; ++i) {
        snprintf(name, sizeof(name), "MaxSubs_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        EXPECT_TRUE(subscribe(fd, i)) << "subscribe " << i;
    }
    EXPECT_FALSE(subscribe(fd, 4)) << "5th subscription must exceed --max-subs 4";

    close(fd);
    close(ctl);
}

/* 5. SET_SUBS 快照：3 个点位，按 id 升序收到 3 帧 NOTIFY，quality 与 GET 一致 */
TEST(RtdbdNotify, SetSubsSnapshotThree)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs3", policy_for_self_range(12), 64, 16));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 10; i <= 12; ++i) {
        snprintf(name, sizeof(name), "Snap_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
    }
    /* 写入初始值 */
    ASSERT_TRUE(write_int32(ctl, 10, 100));
    ASSERT_TRUE(write_int32(ctl, 11, 200));
    ASSERT_TRUE(write_int32(ctl, 12, 300));

    /* SET_SUBS {10,11,12} */
    EXPECT_EQ(set_subs(sub, {10, 11, 12}), RTDBD_ST_OK);

    /* 收到 3 帧 NOTIFY，按 id 升序 */
    rtdbd_notify_t nt;
    int32_t expected_values[] = {100, 200, 300};
    for (int i = 0; i < 3; ++i) {
        uint32_t expected_id = 10 + i;
        ASSERT_TRUE(recv_notify(sub, &nt)) << "missing notify for " << expected_id;
        EXPECT_EQ(nt.point_id, expected_id);
        EXPECT_EQ(nt.type, RTDBD_TYPE_INT32);
        EXPECT_EQ((int32_t)(int64_t)nt.value_bits, expected_values[i]);
        EXPECT_EQ(nt.reserved[0], INDURTDB_QUALITY_GOOD);  /* quality 在 reserved[0] */
    }
    /* 第 4 帧不应存在（短轮询确认） */
    EXPECT_FALSE(recv_notify(sub, &nt, 200));

    close(sub);
    close(ctl);
}

/* 6. SET_SUBS 替换旧订阅集：先 {10,11}，再 {12}，只收到 1 帧；写 10 不再通知 */
TEST(RtdbdNotify, SetSubsReplacesOldSet)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_repl", policy_for_self_range(12), 64, 16));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 10; i <= 12; ++i) {
        snprintf(name, sizeof(name), "Repl_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
    }
    ASSERT_TRUE(write_int32(ctl, 10, 100));
    ASSERT_TRUE(write_int32(ctl, 11, 200));
    ASSERT_TRUE(write_int32(ctl, 12, 300));

    /* 第一次 SET_SUBS {10,11} */
    EXPECT_EQ(set_subs(sub, {10, 11}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 11u);
    EXPECT_FALSE(recv_notify(sub, &nt, 200));

    /* 第二次 SET_SUBS {12} 替换 */
    EXPECT_EQ(set_subs(sub, {12}), RTDBD_ST_OK);
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 12u);
    EXPECT_FALSE(recv_notify(sub, &nt, 200));

    /* 写 10 不应再产生通知（已被替换） */
    ASSERT_TRUE(write_int32(ctl, 10, 999));
    EXPECT_FALSE(recv_notify(sub, &nt, 500));

    close(sub);
    close(ctl);
}

/* 7. SET_SUBS 拒绝未注册 id：{10,99} → NOT_FOUND，旧订阅 {10} 仍收通知 */
TEST(RtdbdNotify, SetSubsRejectsUnregistered)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_unreg", policy_for_self_range(10), 64, 16));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    snprintf(name, sizeof(name), "Unreg_10");
    ASSERT_TRUE(create_point(ctl, 10, name));

    /* 先用 SUBSCRIBE 订阅 10（旧接口） */
    ASSERT_TRUE(subscribe(sub, 10));
    /* 写入初始值，订阅后写入才会收到通知 */
    ASSERT_TRUE(write_int32(ctl, 10, 100));
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 100);

    /* SET_SUBS {10, 99} → 99 未注册，应返回 NOT_FOUND */
    EXPECT_EQ(set_subs(sub, {10, 99}), RTDBD_ST_NOT_FOUND);

    /* 旧订阅 {10} 应保持不变，写 10 仍收到通知 */
    ASSERT_TRUE(write_int32(ctl, 10, 200));
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 200);

    close(sub);
    close(ctl);
}

/* 7b. SET_SUBS 拒绝策略不允许的 id：{10,11} → DENIED；旧订阅 {10} 不变 */
TEST(RtdbdNotify, SetSubsRejectsDenied)
{
    Proc p;
    const std::string cfg =
        "points:\n"
        "  - id: 10\n"
        "    name: \"Cfg_Allow10\"\n"
        "    type: int32\n"
        "    access: 3\n"
        "  - id: 11\n"
        "    name: \"Cfg_Deny11\"\n"
        "    type: int32\n"
        "    access: 3\n";
    ASSERT_TRUE(p.start_cfg("setsubs_denied", policy_for_self_range(10), 64, 16, cfg));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    /* 先建立旧订阅 {10} */
    EXPECT_EQ(set_subs(sub, {10}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);

    /* {10,11} 中 11 被 policy 拒绝 → DENIED，旧订阅保持不变 */
    EXPECT_EQ(set_subs(sub, {10, 11}), RTDBD_ST_DENIED);
    ASSERT_TRUE(write_int32(ctl, 10, 200));
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 200);

    close(sub);
    close(ctl);
}

/* 7c. SET_SUBS count 超过 --max-subs → BAD_REQUEST，旧订阅不变 */
TEST(RtdbdNotify, SetSubsCountOverMaxSubs)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_overmax", policy_for_self_range(14), 64, 4));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 10; i <= 14; ++i) {
        snprintf(name, sizeof(name), "OverMax_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
    }

    EXPECT_EQ(set_subs(sub, {10}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);

    EXPECT_EQ(set_subs(sub, {10, 11, 12, 13, 14}), RTDBD_ST_BAD_REQUEST);

    ASSERT_TRUE(write_int32(ctl, 10, 71));
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 71);

    close(sub);
    close(ctl);
}

/* 8. SET_SUBS 拒绝重复 id：{10,10} → BAD_REQUEST */
TEST(RtdbdNotify, SetSubsRejectsDuplicate)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_dup", policy_for_self_range(10), 64, 16));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    snprintf(name, sizeof(name), "Dup_10");
    ASSERT_TRUE(create_point(ctl, 10, name));
    ASSERT_TRUE(write_int32(ctl, 10, 100));

    /* SET_SUBS {10, 10} → 重复，应返回 BAD_REQUEST */
    EXPECT_EQ(set_subs(sub, {10, 10}), RTDBD_ST_BAD_REQUEST);

    close(sub);
    close(ctl);
}

/* 9. SET_SUBS 空集合清空订阅：{} → OK，之后写旧点无通知 */
TEST(RtdbdNotify, SetSubsEmptyClears)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_empty", policy_for_self_range(10), 64, 16));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    snprintf(name, sizeof(name), "Empty_10");
    ASSERT_TRUE(create_point(ctl, 10, name));

    /* 先订阅 10 */
    ASSERT_TRUE(subscribe(sub, 10));
    /* 写入初始值，订阅后写入才会收到通知 */
    ASSERT_TRUE(write_int32(ctl, 10, 100));
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt)); EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 100);

    /* SET_SUBS {} 清空 */
    EXPECT_EQ(set_subs(sub, {}), RTDBD_ST_OK);
    EXPECT_FALSE(recv_notify(sub, &nt, 200));

    /* 写 10 不应再产生通知 */
    ASSERT_TRUE(write_int32(ctl, 10, 200));
    EXPECT_FALSE(recv_notify(sub, &nt, 500));

    close(sub);
    close(ctl);
}

/* 10. SET_SUBS 500 点快照全部收到，health.notify_drop == 0 */
TEST(RtdbdNotify, SetSubsSnapshot500Fits)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_500", policy_for_self_range(599), 600, 1024));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 100; i < 600; ++i) {
        snprintf(name, sizeof(name), "P500_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
        ASSERT_TRUE(write_int32(ctl, i, (int32_t)i));
    }

    std::vector<uint32_t> ids;
    ids.reserve(500);
    for (uint32_t i = 100; i < 600; ++i) ids.push_back(i);

    EXPECT_EQ(set_subs(sub, ids), RTDBD_ST_OK);

    rtdbd_notify_t nt;
    uint32_t received = 0;
    for (uint32_t i = 0; i < 500; ++i) {
        if (recv_notify(sub, &nt, 5000)) {
            EXPECT_EQ(nt.point_id, 100 + i);
            EXPECT_EQ((int32_t)(int64_t)nt.value_bits, (int32_t)(100 + i));
            EXPECT_EQ(nt.reserved[0], INDURTDB_QUALITY_GOOD);
            received++;
        } else {
            break;
        }
    }
    EXPECT_EQ(received, 500u) << "should receive all 500 snapshots";

    /* 查询 health 确认 notify_drop == 0 */
    int health_fd = connect_to(p.sock);
    ASSERT_GE(health_fd, 0);
    rtdbd_req_hdr_t hreq;
    memset(&hreq, 0, sizeof(hreq));
    hreq.magic = RTDBD_MAGIC;
    hreq.version = RTDBD_PROTO_VERSION;
    hreq.opcode = RTDBD_OP_HEALTH;
    hreq.payload_len = 0;
    ASSERT_TRUE(send(health_fd, &hreq, sizeof(hreq), 0) == (ssize_t)sizeof(hreq));
    rtdbd_resp_hdr_t hresp;
    ASSERT_TRUE(recv(health_fd, &hresp, sizeof(hresp), MSG_WAITALL) == (ssize_t)sizeof(hresp));
    EXPECT_EQ(hresp.status, RTDBD_ST_OK);
    rtdbd_health_t health;
    ASSERT_TRUE(recv(health_fd, &health, sizeof(health), MSG_WAITALL) == (ssize_t)sizeof(health));
    EXPECT_EQ(health.notify_drop, 0ull) << "notify_drop should be 0 after snapshot replacement";
    close(health_fd);

    close(sub);
    close(ctl);
}

/* 11. SET_SUBS 替换不计入 notify_drop：连续 10 次替换 50 点页，notify_drop 不增长 */
TEST(RtdbdNotify, SetSubsReplaceDoesNotCountDrop)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_nodrop", policy_for_self_range(599), 600, 1024));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    char name[32];
    for (uint32_t i = 100; i < 600; ++i) {
        snprintf(name, sizeof(name), "NDrop_%u", i);
        ASSERT_TRUE(create_point(ctl, i, name)) << "create point " << i;
        ASSERT_TRUE(write_int32(ctl, i, (int32_t)i));
    }

    /* 先查询初始 notify_drop */
    int health_fd = connect_to(p.sock);
    ASSERT_GE(health_fd, 0);
    rtdbd_req_hdr_t hreq;
    memset(&hreq, 0, sizeof(hreq));
    hreq.magic = RTDBD_MAGIC;
    hreq.version = RTDBD_PROTO_VERSION;
    hreq.opcode = RTDBD_OP_HEALTH;
    hreq.payload_len = 0;
    ASSERT_TRUE(send(health_fd, &hreq, sizeof(hreq), 0) == (ssize_t)sizeof(hreq));
    rtdbd_resp_hdr_t hresp;
    ASSERT_TRUE(recv(health_fd, &hresp, sizeof(hresp), MSG_WAITALL) == (ssize_t)sizeof(hresp));
    rtdbd_health_t health_before;
    ASSERT_TRUE(recv(health_fd, &health_before, sizeof(health_before), MSG_WAITALL) == (ssize_t)sizeof(health_before));
    close(health_fd);

    /* 连续 10 次替换 50 点页 */
    for (int round = 0; round < 10; ++round) {
        std::vector<uint32_t> ids;
        ids.reserve(50);
        uint32_t base = 100 + (round % 10) * 50;
        for (uint32_t i = 0; i < 50; ++i) ids.push_back(base + i);
        EXPECT_EQ(set_subs(sub, ids), RTDBD_ST_OK);

        /* 排空快照 */
        rtdbd_notify_t nt;
        for (uint32_t i = 0; i < 50; ++i) {
            recv_notify(sub, &nt, 2000);
        }
    }

    /* 再次查询 health，notify_drop 应不变 */
    health_fd = connect_to(p.sock);
    ASSERT_GE(health_fd, 0);
    ASSERT_TRUE(send(health_fd, &hreq, sizeof(hreq), 0) == (ssize_t)sizeof(hreq));
    ASSERT_TRUE(recv(health_fd, &hresp, sizeof(hresp), MSG_WAITALL) == (ssize_t)sizeof(hresp));
    rtdbd_health_t health_after;
    ASSERT_TRUE(recv(health_fd, &health_after, sizeof(health_after), MSG_WAITALL) == (ssize_t)sizeof(health_after));
    close(health_fd);

    EXPECT_EQ(health_after.notify_drop, health_before.notify_drop)
        << "SET_SUBS replacement should not increase notify_drop";

    close(sub);
    close(ctl);
}

/* 12. 超时只通知一次：timeout_ms=50，1 Hz 扫描后收到 1 帧 TIMEOUT */
TEST(RtdbdNotify, TimeoutTransitionNotifiesOnce)
{
    Proc p;
    const std::string cfg =
        "points:\n"
        "  - id: 10\n"
        "    name: \"T_Once\"\n"
        "    type: int32\n"
        "    access: 3\n"
        "    timeout_ms: 50\n";
    ASSERT_TRUE(p.start_cfg("timeout_once", policy_for_self_range(10), 64, 16, cfg));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    EXPECT_EQ(set_subs(sub, {10}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt));       /* 快照 */
    EXPECT_EQ(nt.point_id, 10u);

    ASSERT_TRUE(write_int32(ctl, 10, 123));   /* 建立时间戳 */
    ASSERT_TRUE(recv_notify(sub, &nt));       /* 写入通知 GOOD */
    EXPECT_EQ(INDURTDB_QUALITY_BASE(nt.reserved[0]), INDURTDB_QUALITY_GOOD);

    ASSERT_TRUE(recv_notify(sub, &nt, 3500)) << "应在一个扫描周期内收到 TIMEOUT";
    EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(nt.reserved[0]), INDURTDB_QUALITY_TIMEOUT);

    /* 同一超时状态不重复推送 */
    EXPECT_FALSE(recv_notify(sub, &nt, 1500)) << "TIMEOUT 状态不应重复通知";

    close(sub);
    close(ctl);
}

/* 13. 写路径恢复：TIMEOUT 后写入，立即收到 GOOD 通知 */
TEST(RtdbdNotify, WriteRecoversTimeout)
{
    Proc p;
    const std::string cfg =
        "points:\n"
        "  - id: 10\n"
        "    name: \"T_Recover\"\n"
        "    type: int32\n"
        "    access: 3\n"
        "    timeout_ms: 50\n";
    ASSERT_TRUE(p.start_cfg("timeout_recover", policy_for_self_range(10), 64, 16, cfg));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);

    EXPECT_EQ(set_subs(sub, {10}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt));
    ASSERT_TRUE(write_int32(ctl, 10, 1));
    ASSERT_TRUE(recv_notify(sub, &nt));       /* GOOD */
    ASSERT_TRUE(recv_notify(sub, &nt, 3500)); /* TIMEOUT */
    EXPECT_EQ(INDURTDB_QUALITY_BASE(nt.reserved[0]), INDURTDB_QUALITY_TIMEOUT);

    ASSERT_TRUE(write_int32(ctl, 10, 2));     /* 恢复写 */
    ASSERT_TRUE(recv_notify(sub, &nt));       /* GOOD */
    EXPECT_EQ(nt.point_id, 10u);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(nt.reserved[0]), INDURTDB_QUALITY_GOOD);
    EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 2);

    close(sub);
    close(ctl);
}

/* 14. 超时只通知订阅者：未订阅连接收不到 */
TEST(RtdbdNotify, TimeoutNotifiesOnlySubscribers)
{
    Proc p;
    const std::string cfg =
        "points:\n"
        "  - id: 10\n"
        "    name: \"T_OnlySub\"\n"
        "    type: int32\n"
        "    access: 3\n"
        "    timeout_ms: 50\n";
    ASSERT_TRUE(p.start_cfg("timeout_onlysub", policy_for_self_range(10), 64, 16, cfg));

    int ctl = connect_to(p.sock);
    int sub = connect_to(p.sock);
    int other = connect_to(p.sock);
    ASSERT_GE(ctl, 0);
    ASSERT_GE(sub, 0);
    ASSERT_GE(other, 0);

    EXPECT_EQ(set_subs(sub, {10}), RTDBD_ST_OK);
    rtdbd_notify_t nt;
    ASSERT_TRUE(recv_notify(sub, &nt));
    ASSERT_TRUE(write_int32(ctl, 10, 5));
    ASSERT_TRUE(recv_notify(sub, &nt));       /* GOOD */
    ASSERT_TRUE(recv_notify(sub, &nt, 3500)); /* TIMEOUT */
    EXPECT_EQ(INDURTDB_QUALITY_BASE(nt.reserved[0]), INDURTDB_QUALITY_TIMEOUT);
    EXPECT_FALSE(recv_notify(other, &nt, 1000)) << "未订阅连接不应收到超时通知";

    close(other);
    close(sub);
    close(ctl);
}

/* 13. CAPS 上报 v3.11 容量：默认 128/32/10000/1024/1024。 */
TEST(RtdbdNotify, CapsReportsCapacity)
{
    Proc p;
    ASSERT_TRUE(p.start_with("caps_defaults", policy_for_self(), 64, 10000));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_caps_t caps;
    ASSERT_TRUE(caps_exchange(fd, &caps));
    EXPECT_EQ(caps.proto_version, RTDBD_PROTO_VERSION);
    EXPECT_EQ(caps.max_clients, 128u);
    EXPECT_EQ(caps.initial_clients, 32u);
    EXPECT_EQ(caps.max_subs_per_conn, 10000u);
    EXPECT_EQ(caps.initial_subs_per_conn, 1024u);
    EXPECT_EQ(caps.max_batch, 1024u);
    EXPECT_EQ(caps.flags & (RTDBD_CAPS_F_BATCH_READ |
                            RTDBD_CAPS_F_BATCH_WRITE |
                            RTDBD_CAPS_F_SNAPSHOT_DONE),
              (RTDBD_CAPS_F_BATCH_READ |
               RTDBD_CAPS_F_BATCH_WRITE |
               RTDBD_CAPS_F_SNAPSHOT_DONE));

    /* 重复协商幂等。 */
    rtdbd_caps_t again;
    ASSERT_TRUE(caps_exchange(fd, &again));
    EXPECT_EQ(again.max_subs_per_conn, caps.max_subs_per_conn);

    close(fd);
}

/* 14. 10,000 订阅：全部快照后收到 SNAPSHOT_DONE，count 精确。 */
TEST(RtdbdNotify, SetSubsSnapshotDoneTenThousand)
{
    const uint32_t N = 10000;
    Proc p;
    ASSERT_TRUE(p.start_cfg("setsubs_10k", policy_for_self_range(N),
                            N, N, yaml_points(N)));

    int sub = connect_to(p.sock);
    ASSERT_GE(sub, 0);

    rtdbd_caps_t caps;
    ASSERT_TRUE(caps_exchange(sub, &caps));
    EXPECT_EQ(caps.max_subs_per_conn, N);

    std::vector<uint32_t> ids;
    ids.reserve(N);
    for (uint32_t i = 0; i < N; ++i) ids.push_back(i);

    EXPECT_EQ(set_subs(sub, ids), RTDBD_ST_OK);

    rtdbd_notify_t nt;
    for (uint32_t i = 0; i < N; ++i) {
        ASSERT_TRUE(recv_notify(sub, &nt, 5000)) << "snapshot frame " << i;
        EXPECT_EQ(nt.point_id, i);
        EXPECT_EQ((int32_t)(int64_t)nt.value_bits, 0);
        EXPECT_NE(nt.reserved[1] & RTDBD_NOTIFY_F_SNAPSHOT, 0u);
    }

    rtdbd_snapshot_done_t done;
    ASSERT_TRUE(recv_done(sub, &done, 5000));
    EXPECT_EQ(done.flags, RTDBD_SNAPSHOT_DONE_F_COMPLETE);
    EXPECT_EQ(done.count, N);

    close(sub);
}

/* 15. 未协商 CAPS 的旧客户端：收快照，但不收 SNAPSHOT_DONE。 */
TEST(RtdbdNotify, LegacyClientNeverGetsSnapshotDone)
{
    Proc p;
    ASSERT_TRUE(p.start_cfg("legacy_nodone", policy_for_self_range(3),
                            64, 10000, yaml_points(3)));

    int sub = connect_to(p.sock);
    ASSERT_GE(sub, 0);

    EXPECT_EQ(set_subs(sub, {0, 1, 2}), RTDBD_ST_OK);

    rtdbd_notify_t nt;
    for (uint32_t i = 0; i < 3; ++i) {
        ASSERT_TRUE(recv_notify(sub, &nt, 5000));
        EXPECT_EQ(nt.point_id, i);
    }

    struct pollfd pfd;
    pfd.fd = sub;
    pfd.events = POLLIN;
    EXPECT_EQ(poll(&pfd, 1, 300), 0) << "legacy client must not receive SNAPSHOT_DONE";

    close(sub);
}

/* 16. 空订阅集也发 SNAPSHOT_DONE(count=0)，用于监控台建立空快照屏障。 */
TEST(RtdbdNotify, EmptySetSubsSendsSnapshotDone)
{
    Proc p;
    ASSERT_TRUE(p.start_with("setsubs_empty_done", policy_for_self(), 64, 10000));

    int sub = connect_to(p.sock);
    ASSERT_GE(sub, 0);

    rtdbd_caps_t caps;
    ASSERT_TRUE(caps_exchange(sub, &caps));
    EXPECT_EQ(set_subs(sub, {}), RTDBD_ST_OK);

    rtdbd_snapshot_done_t done;
    ASSERT_TRUE(recv_done(sub, &done, 2000));
    EXPECT_EQ(done.flags, RTDBD_SNAPSHOT_DONE_F_COMPLETE);
    EXPECT_EQ(done.count, 0u);

    close(sub);
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
