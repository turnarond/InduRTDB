/**
 * @file test_rtdbd_delta.cpp
 * @brief 运行时点位变更持久化集成测试（v3.7 主题B B4）
 *
 * 覆盖：
 *   - CreatedPointSurvivesRestart     CREATE 后重启，点位仍在（需 --delta-file）
 *   - DeleteAndRenameSurviveRestart   DELETE / RENAME 同样持久化
 *   - WithoutDeltaFilePointsAreLost   未指定 --delta-file 时行为同现状（重启即丢）
 *   - TrailingFragmentIsTolerated     delta 末尾残片可容忍，服务仍能启动
 */
#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include <signal.h>
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
    if (f) { fwrite(content.data(), 1, content.size(), f); fclose(f); }
    return path;
}

/* 清理某实例的共享内存段。
 *
 * 关键设计与踩坑记录：
 * 1. **启动前也要清** —— 清理若只放在用例末尾，用例一旦在 ASSERT 处中止就不会执行，
 *    遗留的段会让下一次运行 attach 失败（"shm init failed"），
 *    表现为"失败一次后持续失败"的假故障。
 * 2. **只 shm_unlink，不调 indurtdb_initialize** —— 早期版本先 initialize 再 detach
 *    再 unlink，结果：initialize 会把测试进程的 g_default 置为已初始化，
 *    后续调用直接返回"already initialized"而成为 no-op（段清不掉）；
 *    且会在 rtdbd 仍在运行时把它的段删掉。直接 unlink 名字最干净，
 *    不触碰库状态，也无所谓谁持有映射。 */
void drop_segment(const std::string& instance)
{
    std::string n = "/indurtdb_" + instance;
    shm_unlink(n.c_str());   /* 不存在时返回 -1，忽略 */
}

/* 启动 rtdbd（可选 delta 文件）。 */
struct RtdbdProc {
    std::string sock;
    std::string instance;
    std::string policy_path;
    std::string config_path;
    std::string delta_path;
    pid_t       pid = -1;

    ~RtdbdProc()
    {
        stop();
        drop_segment(instance);   /* 析构兜底：即使断言失败也不留段 */
    }

    bool start(const std::string& tag, const std::string& pol,
               const std::string& cfg, const std::string& delta)
    {
        sock        = "/tmp/indurtdb_delta_" + tag + ".sock";
        instance    = "delta_test_" + tag;
        policy_path = make_tmp_file("delta_policy_" + tag + ".txt", pol);
        config_path = make_tmp_file("delta_cfg_" + tag + ".yaml", cfg);
        delta_path  = delta.empty() ? "" : "/tmp/irt_delta_" + tag + ".bin";

        drop_segment(instance);   /* 先清历史遗留段，避免 attach 失败 */
        unlink(sock.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(RTDBD_BIN));
            argv.push_back(const_cast<char*>("--socket"));   argv.push_back(const_cast<char*>(sock.c_str()));
            argv.push_back(const_cast<char*>("--instance")); argv.push_back(const_cast<char*>(instance.c_str()));
            argv.push_back(const_cast<char*>("--policy"));   argv.push_back(const_cast<char*>(policy_path.c_str()));
            argv.push_back(const_cast<char*>("--config"));   argv.push_back(const_cast<char*>(config_path.c_str()));
            argv.push_back(const_cast<char*>("--max-points")); argv.push_back(const_cast<char*>("64"));
            argv.push_back(const_cast<char*>("--max-subs"));   argv.push_back(const_cast<char*>("4"));
            if (!delta_path.empty()) {
                argv.push_back(const_cast<char*>("--delta-file"));
                argv.push_back(const_cast<char*>(delta_path.c_str()));
            }
            argv.push_back(nullptr);
            execv(RTDBD_BIN, argv.data());
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
            for (int i = 0; i < 500; ++i) {
                int st = 0;
                if (waitpid(pid, &st, WNOHANG) == pid) { reaped = 1; break; }
                usleep(10000);
            }
            if (!reaped) { kill(pid, SIGKILL); int st = 0; waitpid(pid, &st, 0); }
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

std::string policy_allow()
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", (unsigned long)getuid());
    return std::string(buf);
}

const char* kConfig =
    "points:\n"
    "  - id: 1\n"
    "    name: \"Base_Point\"\n"
    "    type: int32\n"
    "    access: 3\n";

/* 经协议建点，返回状态码 */
int create_point(int fd, uint32_t id, const char* name)
{
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id = id;
    c.type     = RTDBD_TYPE_INT32;
    c.access   = 3;
    strncpy(c.name, name, sizeof(c.name) - 1);
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body)) return -1;
    return (int)resp.status;
}

int rename_point(int fd, uint32_t id, const char* name)
{
    rtdbd_rename_req_t r;
    memset(&r, 0, sizeof(r));
    r.point_id = id;
    strncpy(r.name, name, sizeof(r.name) - 1);
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_RENAME_POINT, &r, sizeof(r), &resp, &body)) return -1;
    return (int)resp.status;
}

int delete_point(int fd, uint32_t id)
{
    rtdbd_delete_req_t d;
    memset(&d, 0, sizeof(d));
    d.point_id = id;
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_DELETE_POINT, &d, sizeof(d), &resp, &body)) return -1;
    return (int)resp.status;
}

/* FIND_BY_NAME：能查到说明点位（含索引）已恢复 */
bool find_by_name(int fd, const char* name, uint32_t* out_id)
{
    rtdbd_find_req_t f;
    memset(&f, 0, sizeof(f));
    strncpy(f.name, name, sizeof(f.name) - 1);
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_FIND_BY_NAME, &f, sizeof(f), &resp, &body)) return false;
    if (resp.status != RTDBD_ST_OK || body.size() != sizeof(rtdbd_find_resp_t)) return false;
    rtdbd_find_resp_t r;
    memcpy(&r, body.data(), sizeof(r));
    *out_id = r.point_id;
    return true;
}

} // namespace

/* 1. CREATE 后重启，点位仍在（含 name→id 索引） */
TEST(RtdbdDelta, CreatedPointSurvivesRestart)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("create", policy_allow(), kConfig, "yes"));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(create_point(fd, 20, "Runtime_P20"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    /* 重启：同 instance + 同 delta 文件 */
    RtdbdProc p2;
    ASSERT_TRUE(p2.start("create", policy_allow(), kConfig, "yes"));
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);

    uint32_t id = 0;
    EXPECT_TRUE(find_by_name(fd2, "Runtime_P20", &id)) << "runtime point should survive restart";
    EXPECT_EQ(id, 20u);
    /* base 点位也应仍在 */
    uint32_t bid = 0;
    EXPECT_TRUE(find_by_name(fd2, "Base_Point", &bid));

    close(fd2);
    p2.stop();
    unlink("/tmp/irt_delta_create.bin");
    drop_segment("delta_test_create");
}

/* 2. DELETE 与 RENAME 同样持久化 */
TEST(RtdbdDelta, DeleteAndRenameSurviveRestart)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("mutate", policy_allow(), kConfig, "yes"));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(create_point(fd, 21, "P21"), (int)RTDBD_ST_OK);
    ASSERT_EQ(create_point(fd, 22, "P22"), (int)RTDBD_ST_OK);
    ASSERT_EQ(delete_point(fd, 21), (int)RTDBD_ST_OK);
    ASSERT_EQ(rename_point(fd, 22, "P22_renamed"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    RtdbdProc p2;
    ASSERT_TRUE(p2.start("mutate", policy_allow(), kConfig, "yes"));
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);

    uint32_t id = 0;
    EXPECT_FALSE(find_by_name(fd2, "P21", &id)) << "deleted point must stay deleted";
    EXPECT_TRUE(find_by_name(fd2, "P22_renamed", &id)) << "rename must persist";
    EXPECT_EQ(id, 22u);

    close(fd2);
    p2.stop();
    unlink("/tmp/irt_delta_mutate.bin");
    drop_segment("delta_test_mutate");
}

/* 3. 未指定 --delta-file：行为与现状一致（运行时点位重启即丢） */
TEST(RtdbdDelta, WithoutDeltaFilePointsAreLost)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("nodelta", policy_allow(), kConfig, ""));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(create_point(fd, 23, "P23"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    RtdbdProc p2;
    ASSERT_TRUE(p2.start("nodelta", policy_allow(), kConfig, ""));
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);

    uint32_t id = 0;
    EXPECT_FALSE(find_by_name(fd2, "P23", &id))
        << "without --delta-file, runtime points are lost (unchanged behavior)";
    uint32_t bid = 0;
    EXPECT_TRUE(find_by_name(fd2, "Base_Point", &bid)) << "base config points still load";

    close(fd2);
    p2.stop();
    drop_segment("delta_test_nodelta");
}

/* 4. base 配置点的 DELETE / RENAME 必须跨重启保留。
 *    护栏意义：曾因 rtdbd 把 delta 回放放在 load_config **之前**，
 *    导致 base 无条件覆盖槽位 —— 运行时 DELETE 的点被"复活"、RENAME 被覆盖回原名，
 *    而 base 点恰恰是最常见的操作对象。本用例钉死正确顺序。 */
TEST(RtdbdDelta, BasePointMutationsSurviveRestart)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("basemut", policy_allow(), kConfig, "yes"));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    /* 删除 base 配置的 id=1（Base_Point），并新建+改名一个运行时点 */
    ASSERT_EQ(delete_point(fd, 1), (int)RTDBD_ST_OK);
    ASSERT_EQ(create_point(fd, 30, "P30"), (int)RTDBD_ST_OK);
    ASSERT_EQ(rename_point(fd, 30, "P30_renamed"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    RtdbdProc p2;
    ASSERT_TRUE(p2.start("basemut", policy_allow(), kConfig, "yes"));
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);

    uint32_t id = 0;
    EXPECT_FALSE(find_by_name(fd2, "Base_Point", &id))
        << "runtime DELETE of a base-config point must survive restart";
    EXPECT_TRUE(find_by_name(fd2, "P30_renamed", &id)) << "rename must persist";
    EXPECT_EQ(id, 30u);

    close(fd2);
    p2.stop();
    unlink("/tmp/irt_delta_basemut.bin");
}

/* 5. 回放幂等：同一 delta 回放两次，不应重复追加记录使文件增长。
 *    护栏意义：delta_apply 走真实 CRUD，若回放期间不抑制 delta_log，
 *    会把记录写回"正在读的同一文件" —— 无限循环并打满磁盘。 */
TEST(RtdbdDelta, ReplayIsIdempotentAndDoesNotGrow)
{
    const std::string d = "/tmp/irt_delta_idem.bin";
    unlink(d.c_str());

    /* 第一次启动：建点产生记录 */
    RtdbdProc p;
    ASSERT_TRUE(p.start("idem", policy_allow(), kConfig, "yes"));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(create_point(fd, 40, "P40"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    struct stat st1;
    ASSERT_EQ(stat(d.c_str(), &st1), 0);
    const off_t size1 = st1.st_size;
    ASSERT_GT(size1, 0);

    /* 第二次启动：只回放、不做任何变更 —— 文件大小必须不变 */
    RtdbdProc p2;
    ASSERT_TRUE(p2.start("idem", policy_allow(), kConfig, "yes"));
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);
    uint32_t id = 0;
    EXPECT_TRUE(find_by_name(fd2, "P40", &id)) << "point still present after 2nd start";
    close(fd2);
    p2.stop();

    struct stat st2;
    ASSERT_EQ(stat(d.c_str(), &st2), 0);
    EXPECT_EQ(st2.st_size, size1)
        << "replay must not re-append records (would loop and fill the disk)";

    unlink(d.c_str());
}

/* 6. delta 末尾残片可容忍：服务仍能启动，且残片前的记录生效 */
TEST(RtdbdDelta, TrailingFragmentIsTolerated)
{
    const std::string d = "/tmp/irt_delta_frag.bin";
    /* 先正常建点生成完整记录 */
    RtdbdProc p;
    ASSERT_TRUE(p.start("frag", policy_allow(), kConfig, "yes"));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(create_point(fd, 24, "P24"), (int)RTDBD_ST_OK);
    close(fd);
    p.stop();

    /* 人为追加 30B 残片（模拟写到一半崩溃） */
    FILE* f = fopen(d.c_str(), "ab");
    ASSERT_NE(f, nullptr);
    std::vector<uint8_t> junk(30, 0xAB);
    fwrite(junk.data(), 1, junk.size(), f);
    fclose(f);

    RtdbdProc p2;
    ASSERT_TRUE(p2.start("frag", policy_allow(), kConfig, "yes"))
        << "trailing fragment must not prevent startup";
    int fd2 = connect_to(p2.sock);
    ASSERT_GE(fd2, 0);

    uint32_t id = 0;
    EXPECT_TRUE(find_by_name(fd2, "P24", &id)) << "records before the fragment still apply";
    EXPECT_EQ(id, 24u);

    close(fd2);
    p2.stop();
    unlink(d.c_str());
    drop_segment("delta_test_frag");
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
