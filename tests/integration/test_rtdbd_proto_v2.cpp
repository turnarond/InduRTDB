/**
 * @file test_rtdbd_proto_v2.cpp
 * @brief rtdbd / irtcli 协议 v2 集成测试（T9 红→绿）
 *
 * 覆盖 T9 验收用例：
 *   - FindByName                客户端经服务端按名查到 id（共享索引）
 *   - MetaWriteRequiresAuth     未授权 uid 写元数据被拒（默认拒绝）
 *   - MetaWriteAuthorizedSucceeds  授权 uid 写元数据成功且可读回
 *   - RejectsV1Client           协议版本不匹配（v1 客户端）即断连，不静默
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

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
        sock     = "/tmp/indurtdb_proto2_" + tag + ".sock";
        instance = "proto2_test_" + tag;
        policy_path = make_tmp_file("proto2_policy_" + tag + ".txt", policy_body);
        config_path = make_tmp_file("proto2_cfg_" + tag + ".yaml", config_body);

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

/* 点位配置：注册两个带名的点位，供 FIND_BY_NAME 端到端可用 */
const char* kConfig =
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

std::string policy_for_uid(unsigned long uid)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:0:63\n", uid);
    return std::string(buf);
}

/* 裸协议交互：发请求头+负载，收响应头+全部负载体。返回是否成功收完。 */
bool raw_exchange(int fd, uint16_t op, const void* payload, uint32_t plen,
                  rtdbd_resp_hdr_t* resp, std::vector<uint8_t>* body)
{
    rtdbd_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic        = RTDBD_MAGIC;
    req.version      = RTDBD_PROTO_VERSION;
    req.opcode       = op;
    req.payload_len  = plen;
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

} // namespace

/* 1. 按名查找：客户端经服务端查到 id（共享索引） */
TEST(ProtoV2, FindByName)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("find", policy_for_uid((unsigned long)getuid()), kConfig));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, NULL, NULL), IRTCLI_OK);

    uint32_t id = 0;
    EXPECT_EQ(irtcli_find_by_name(&c, "AHU_01.Supply_Temp", &id), IRTCLI_OK);
    EXPECT_EQ(id, 10u);

    uint32_t id2 = 0;
    EXPECT_EQ(irtcli_find_by_name(&c, "Pump_Start_CMD", &id2), IRTCLI_OK);
    EXPECT_EQ(id2, 11u);

    /* 不存在的名应返回 NOT_FOUND，而非成功或崩溃 */
    uint32_t missing = 0;
    EXPECT_EQ(irtcli_find_by_name(&c, "NoSuch.Point", &missing), IRTCLI_ERR_NOT_FOUND);

    irtcli_close(&c);
    p.stop();
}

/* 2. 元数据写鉴权：默认策略拒绝一切 uid */
TEST(ProtoV2, MetaWriteRequiresAuth)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("denymeta", "# no rules\n", kConfig));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, NULL, NULL), IRTCLI_OK);

    indurtdb_meta_t m;
    memset(&m, 0, sizeof(m));
    m.eur_min = -10.0;
    m.eur_max = 50.0;
    m.deadband = 0.5f;
    m.flags = 0x3u;

    EXPECT_EQ(irtcli_set_meta(&c, 10, &m), IRTCLI_ERR_DENIED)
        << "未授权 uid 写元数据必须被拒（SO_PEERCRED 鉴权生效）";

    irtcli_close(&c);
    p.stop();
}

/* 3. 授权 uid 写元数据成功，且可读回（round-trip） */
TEST(ProtoV2, MetaWriteAuthorizedSucceeds)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("okmeta", policy_for_uid((unsigned long)getuid()), kConfig));

    irtcli_t c;
    ASSERT_EQ(irtcli_init(&c, p.sock.c_str(), 0, NULL, NULL), IRTCLI_OK);

    indurtdb_meta_t m;
    memset(&m, 0, sizeof(m));
    m.eur_min = -10.0;
    m.eur_max = 50.0;
    m.deadband = 0.5f;
    m.flags = 0x3u;

    ASSERT_EQ(irtcli_set_meta(&c, 10, &m), IRTCLI_OK)
        << "授权 uid 写元数据应成功";

    indurtdb_meta_t out;
    memset(&out, 0, sizeof(out));
    ASSERT_EQ(irtcli_get_meta(&c, 10, &out), IRTCLI_OK);
    EXPECT_DOUBLE_EQ(out.eur_min, -10.0);
    EXPECT_DOUBLE_EQ(out.eur_max, 50.0);
    EXPECT_FLOAT_EQ(out.deadband, 0.5f);
    EXPECT_EQ(out.flags, 0x3u);

    irtcli_close(&c);
    p.stop();
}

/* 4. 协议版本不匹配（v1 客户端）即断连，不静默 */
TEST(ProtoV2, RejectsV1Client)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("v1", policy_for_uid((unsigned long)getuid()), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_req_hdr_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.magic   = RTDBD_MAGIC;
    bad.version = RTDBD_PROTO_VERSION - 1; /* 模拟 v1 客户端 */
    bad.opcode  = RTDBD_OP_PING;
    ASSERT_EQ(send(fd, &bad, sizeof(bad), 0), (ssize_t)sizeof(bad));

    /* 服务端应关闭连接：recv 返回 0 */
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0) << "协议版本不匹配（v1 客户端）时服务端应关闭连接";

    close(fd);
    p.stop();
}

/* 5. v3.5 监控：按 id 读取当前值（GET，读免鉴权） */
TEST(ProtoV2, GetPointValue)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("get", policy_for_uid((unsigned long)getuid()), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 用裸 WRITE 同步写入一个值（写需鉴权，与监控读路径分离验证） */
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id    = 10;
    w.type        = (uint8_t)INDURTDB_TYPE_INT32;
    w.value_bits  = (uint64_t)(int32_t)12345;
    w.source_ts_ns = 0;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_WRITE, &w, sizeof(w), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    rtdbd_get_req_t g;
    memset(&g, 0, sizeof(g));
    g.point_id = 10;

    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_GET, &g, sizeof(g), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), sizeof(rtdbd_get_resp_t));

    rtdbd_get_resp_t r;
    memcpy(&r, body.data(), sizeof(r));
    EXPECT_EQ(r.point_id, 10u);
    EXPECT_EQ(r.type, (uint8_t)INDURTDB_TYPE_INT32);
    EXPECT_EQ(r.value_bits, (uint64_t)12345u);

    /* 未注册 id（仍在 max_points 内）应 NOT_FOUND */
    g.point_id = 50;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_GET, &g, sizeof(g), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_NOT_FOUND);

    /* 越界 id 应 NOT_FOUND */
    g.point_id = 999;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_GET, &g, sizeof(g), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_NOT_FOUND);

    close(fd);
    p.stop();
}

/* 6. v3.5 监控：枚举已注册点位（LIST，读免鉴权） */
TEST(ProtoV2, ListPoints)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("list", policy_for_uid((unsigned long)getuid()), kConfig));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_list_req_t l;
    memset(&l, 0, sizeof(l));
    l.max = 0; l.offset = 0; /* 全量 */

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &l, sizeof(l), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), 2u * sizeof(rtdbd_point_info_t));

    rtdbd_point_info_t e[2];
    memcpy(&e[0], body.data(), sizeof(rtdbd_point_info_t));
    memcpy(&e[1], body.data() + sizeof(rtdbd_point_info_t), sizeof(rtdbd_point_info_t));
    EXPECT_EQ(e[0].point_id, 10u);
    EXPECT_EQ(e[1].point_id, 11u);
    EXPECT_EQ(std::string(e[0].name), "AHU_01.Supply_Temp");
    EXPECT_EQ(std::string(e[1].name), "Pump_Start_CMD");
    EXPECT_EQ(e[0].type, (uint8_t)INDURTDB_TYPE_INT32);
    EXPECT_EQ(e[1].type, (uint8_t)INDURTDB_TYPE_BOOL);
    EXPECT_EQ(e[0].access, (uint8_t)INDURTDB_ACCESS_READ_WRITE);
    EXPECT_EQ(e[1].access, (uint8_t)INDURTDB_ACCESS_READ_WRITE);

    /* 分页：单包 max=1 只回 1 条 */
    l.max = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &l, sizeof(l), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);
    EXPECT_EQ(body.size(), 1u * sizeof(rtdbd_point_info_t));

    close(fd);
    p.stop();
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
