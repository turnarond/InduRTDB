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
        sock     = "/tmp/indurtdb_notify_" + tag + ".sock";
        instance = "notify_test_" + tag;
        policy   = "/tmp/notify_policy_" + tag + ".txt";

        FILE* f = fopen(policy.c_str(), "w");
        if (f) { fwrite(policy_body.data(), 1, policy_body.size(), f); fclose(f); }

        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy.c_str(),
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

    ~Proc() { stop(); }

    void stop()
    {
        if (pid > 0) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
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

} // namespace

/* 1. 跨进程订阅者收到变更通知 */
TEST(RtdbdNotify, CrossProcessReceivesChange)
{
    Proc p;
    ASSERT_TRUE(p.start("chg", policy_for_self()));

    int sub = connect_to(p.sock);
    int wri = connect_to(p.sock);
    ASSERT_GE(sub, 0);
    ASSERT_GE(wri, 0);

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
}

/* 2. 大量订阅/退订循环后不泄漏 fd */
TEST(RtdbdNotify, NoFdLeakUnderChurn)
{
    Proc p;
    ASSERT_TRUE(p.start("churn", policy_for_self()));

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

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    if (std::string(RTDBD_BIN).empty()) {
        fprintf(stderr, "RTDBD_BIN not defined\n");
        return 1;
    }
    return RUN_ALL_TESTS();
}
