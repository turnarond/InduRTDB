/**
 * @file test_rtdbd_proto_v3.cpp
 * @brief rtdbd 协议 v3 集成测试（T5 红→绿）
 *
 * 覆盖 T5 验收用例：
 *   - FindByNameWithPartition      经服务端按（分区, 名）查到 id
 *   - ListFilteredByPartition      LIST(partition) 只返回该分区点位
 *   - ListItemIncludesPartition    POINT_INFO 含 partition 字段
 *   - SelectSetsConnectionDefault  OP_SELECT 后本连接默认分区生效
 *   - RejectsV2Client              协议版本不匹配即断连（不静默）
 *   - PolicyPartitionScoped        管控写按（uid, 分区, id 区间）鉴权
 *   - AuditRecordsPartition        审计记录含 partition
 *   - PartitionsWhitelistRejects   配置白名单拒绝非法分区
 */
#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
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
        return start_with(tag, policy_body, config_body, 64, 4);
    }

    bool start_with(const std::string& tag, const std::string& policy_body,
                    const std::string& config_body,
                    uint32_t max_points, uint32_t max_subs)
    {
        sock     = "/tmp/indurtdb_proto3_" + tag + ".sock";
        instance = "proto3_test_" + tag;
        shm_unlink(("/indurtdb_" + instance).c_str());
        policy_path = make_tmp_file("proto3_policy_" + tag + ".txt", policy_body);
        config_path = make_tmp_file("proto3_cfg_" + tag + ".yaml", config_body);

        unlink(sock.c_str());

        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            const std::string mp = std::to_string(max_points);
            const std::string ms = std::to_string(max_subs);
            execl(RTDBD_BIN, "rtdbd",
                  "--socket", sock.c_str(),
                  "--instance", instance.c_str(),
                  "--policy", policy_path.c_str(),
                  "--config", config_path.c_str(),
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

/* 点位配置：注册两个带名的点位（分区 0），供基础测试用 */
const char* kConfigBase =
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

/* 点位配置：带分区白名单与别名 */
const char* kConfigWithWhitelist =
    "partitions: [0, 1]\n"
    "partition_alias: {0: \"default\", 1: \"unitA\"}\n"
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
    snprintf(buf, sizeof(buf), "%lu:*:0:63\n", uid);
    return std::string(buf);
}

std::string policy_for_uid_partition(unsigned long uid, uint32_t partition)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:%u:0:63\n", uid, partition);
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

/* 发送请求但不读响应（用于测试版本不匹配时的断连） */
bool raw_send_only(int fd, uint16_t op, uint16_t version, const void* payload, uint32_t plen)
{
    rtdbd_req_hdr_t req;
    memset(&req, 0, sizeof(req));
    req.magic        = RTDBD_MAGIC;
    req.version      = version;
    req.opcode       = op;
    req.payload_len  = plen;
    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req)) return false;
    if (plen && send(fd, payload, plen, 0) != (ssize_t)plen) return false;
    return true;
}

std::string policy_for_uid_range(unsigned long uid, uint32_t last_id)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%lu:*:0:%u\n", uid, last_id);
    return std::string(buf);
}

bool create_point(int fd, uint32_t id, const char* name, uint32_t partition = 0,
                  uint8_t type = RTDBD_TYPE_INT32)
{
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id  = id;
    c.type      = type;
    c.access    = 3;
    c.partition = partition;
    snprintf(c.name, sizeof(c.name), "%s", name);

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body))
        return false;
    return resp.status == RTDBD_ST_OK;
}

bool write_value(int fd, uint32_t id, int32_t value)
{
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id    = id;
    w.type        = RTDBD_TYPE_INT32;
    w.value_bits  = (uint64_t)(int64_t)value;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_WRITE, &w, sizeof(w), &resp, &body))
        return false;
    return resp.status == RTDBD_ST_OK;
}

bool batch_read(int fd, const std::vector<uint32_t>& ids,
                std::vector<rtdbd_batch_read_item_t>* items,
                uint16_t* status = nullptr)
{
    std::vector<uint8_t> payload(4 + 4 * ids.size());
    uint32_t count = (uint32_t)ids.size();
    memcpy(payload.data(), &count, 4);
    for (size_t i = 0; i < ids.size(); ++i)
        memcpy(payload.data() + 4 + 4 * i, &ids[i], 4);

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_BATCH_READ, payload.data(),
                      (uint32_t)payload.size(), &resp, &body))
        return false;
    if (status) *status = resp.status;
    if (resp.status != RTDBD_ST_OK)
        return true;
    if (body.size() != 4 + ids.size() * sizeof(rtdbd_batch_read_item_t))
        return false;

    uint32_t out_count = 0;
    memcpy(&out_count, body.data(), 4);
    if (out_count != ids.size()) return false;
    items->resize(out_count);
    memcpy(items->data(), body.data() + 4, items->size() * sizeof((*items)[0]));
    return true;
}

bool batch_write(int fd, const std::vector<rtdbd_write_req_t>& writes,
                 std::vector<rtdbd_batch_write_item_t>* items,
                 uint16_t* status = nullptr)
{
    std::vector<uint8_t> payload(4 + writes.size() * sizeof(rtdbd_write_req_t));
    uint32_t count = (uint32_t)writes.size();
    memcpy(payload.data(), &count, 4);
    if (!writes.empty())
        memcpy(payload.data() + 4, writes.data(),
               writes.size() * sizeof(rtdbd_write_req_t));

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    if (!raw_exchange(fd, RTDBD_OP_BATCH_WRITE, payload.data(),
                      (uint32_t)payload.size(), &resp, &body))
        return false;
    if (status) *status = resp.status;
    if (resp.status != RTDBD_ST_OK)
        return true;
    if (body.size() != 4 + writes.size() * sizeof(rtdbd_batch_write_item_t))
        return false;

    uint32_t out_count = 0;
    memcpy(&out_count, body.data(), 4);
    if (out_count != writes.size()) return false;
    items->resize(out_count);
    memcpy(items->data(), body.data() + 4, items->size() * sizeof((*items)[0]));
    return true;
}

rtdbd_write_req_t make_write(uint32_t id, int32_t value)
{
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id   = id;
    w.type       = RTDBD_TYPE_INT32;
    w.value_bits = (uint64_t)(int64_t)value;
    return w;
}

} // namespace

/* 1. 按名查找带分区：同名不同分区解析到不同 id */
TEST(ProtoV3, FindByNameWithPartition)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("findpart", policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 先在分区 1 创建同名点位 id=20 */
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id = 20;
    c.type     = RTDBD_TYPE_INT32;
    c.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c.name, "AHU_01.Supply_Temp", sizeof(c.name) - 1);
    c.partition = 1;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 分区 0 查找 → 应返回配置里的 id=10 */
    rtdbd_find_req_t f0;
    memset(&f0, 0, sizeof(f0));
    strncpy(f0.name, "AHU_01.Supply_Temp", sizeof(f0.name) - 1);
    f0.partition = 0;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_FIND_BY_NAME, &f0, sizeof(f0), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), sizeof(rtdbd_find_resp_t));
    rtdbd_find_resp_t fr0;
    memcpy(&fr0, body.data(), sizeof(fr0));
    EXPECT_EQ(fr0.point_id, 10u);

    /* 分区 1 查找 → 应返回运行时创建的 id=20 */
    rtdbd_find_req_t f1;
    memset(&f1, 0, sizeof(f1));
    strncpy(f1.name, "AHU_01.Supply_Temp", sizeof(f1.name) - 1);
    f1.partition = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_FIND_BY_NAME, &f1, sizeof(f1), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), sizeof(rtdbd_find_resp_t));
    rtdbd_find_resp_t fr1;
    memcpy(&fr1, body.data(), sizeof(fr1));
    EXPECT_EQ(fr1.point_id, 20u);

    close(fd);
    p.stop();
}

/* 2. LIST 按分区过滤：只返回指定分区的点位 */
TEST(ProtoV3, ListFilteredByPartition)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("listpart", policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 在分区 1 创建额外点位 */
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id = 20;
    c.type     = RTDBD_TYPE_INT32;
    c.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c.name, "Partition1.Point", sizeof(c.name) - 1);
    c.partition = 1;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* LIST partition=0 → 只返回分区 0 的点位（id 10, 11） */
    rtdbd_list_req_t l0;
    memset(&l0, 0, sizeof(l0));
    l0.max = 0; l0.offset = 0; l0.partition = 0;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &l0, sizeof(l0), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), 2u * sizeof(rtdbd_point_info_t));
    rtdbd_point_info_t e0[2];
    memcpy(&e0[0], body.data(), sizeof(rtdbd_point_info_t));
    memcpy(&e0[1], body.data() + sizeof(rtdbd_point_info_t), sizeof(rtdbd_point_info_t));
    EXPECT_EQ(e0[0].point_id, 10u);
    EXPECT_EQ(e0[1].point_id, 11u);
    EXPECT_EQ(e0[0].partition, 0u);
    EXPECT_EQ(e0[1].partition, 0u);

    /* LIST partition=1 → 只返回分区 1 的点位（id 20） */
    rtdbd_list_req_t l1;
    memset(&l1, 0, sizeof(l1));
    l1.max = 0; l1.offset = 0; l1.partition = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &l1, sizeof(l1), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), 1u * sizeof(rtdbd_point_info_t));
    rtdbd_point_info_t e1;
    memcpy(&e1, body.data(), sizeof(e1));
    EXPECT_EQ(e1.point_id, 20u);
    EXPECT_EQ(e1.partition, 1u);

    /* LIST partition=ALL → 返回全部 */
    rtdbd_list_req_t lall;
    memset(&lall, 0, sizeof(lall));
    lall.max = 0; lall.offset = 0; lall.partition = RTDBD_PARTITION_ALL;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &lall, sizeof(lall), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), 3u * sizeof(rtdbd_point_info_t));

    close(fd);
    p.stop();
}

/* 3. LIST 返回项包含 partition 字段 */
TEST(ProtoV3, ListItemIncludesPartition)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("listpartfield", policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_list_req_t l;
    memset(&l, 0, sizeof(l));
    l.max = 0; l.offset = 0; l.partition = RTDBD_PARTITION_ALL;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_LIST, &l, sizeof(l), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), 2u * sizeof(rtdbd_point_info_t));

    rtdbd_point_info_t e[2];
    memcpy(&e[0], body.data(), sizeof(rtdbd_point_info_t));
    memcpy(&e[1], body.data() + sizeof(rtdbd_point_info_t), sizeof(rtdbd_point_info_t));

    EXPECT_EQ(e[0].point_id, 10u);
    EXPECT_EQ(e[0].partition, 0u);
    EXPECT_EQ(e[1].point_id, 11u);
    EXPECT_EQ(e[1].partition, 0u);

    close(fd);
    p.stop();
}

/* 4. OP_SELECT 设置连接默认分区：写/元数据操作使用连接默认分区 */
TEST(ProtoV3, SelectSetsConnectionDefault)
{
    /* 策略：uid 仅允许分区 1 的 id 0..999 */
    RtdbdProc p;
    ASSERT_TRUE(p.start("select", policy_for_uid_partition((unsigned long)getuid(), 1), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 连接初始默认分区为 0，SET_META 应被拒（策略只允许分区 1） */
    rtdbd_set_meta_req_t sm;
    memset(&sm, 0, sizeof(sm));
    sm.point_id = 10;
    sm.meta.eur_min = -10.0;
    sm.meta.eur_max = 50.0;
    sm.meta.deadband = 0.5f;
    sm.meta.flags = 0x3u;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SET_META, &sm, sizeof(sm), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    /* OP_SELECT(1) 切换连接默认分区到 1 */
    uint32_t sel_part = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 再次 SET_META → 现在应成功（连接默认分区已变为 1，符合策略） */
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SET_META, &sm, sizeof(sm), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    /* 验证元数据确实写入 */
    rtdbd_meta_req_t gm;
    memset(&gm, 0, sizeof(gm));
    gm.point_id = 10;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_GET_META, &gm, sizeof(gm), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_EQ(body.size(), sizeof(rtdbd_meta_payload_t));
    rtdbd_meta_payload_t mp;
    memcpy(&mp, body.data(), sizeof(mp));
    EXPECT_DOUBLE_EQ(mp.eur_min, -10.0);
    EXPECT_DOUBLE_EQ(mp.eur_max, 50.0);

    close(fd);
    p.stop();
}

/* 5. 协议版本不匹配（v2 客户端）即断连，不静默 */
TEST(ProtoV3, RejectsV2Client)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("v2reject", policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 发送 version=2 的 PING 请求 */
    ASSERT_TRUE(raw_send_only(fd, RTDBD_OP_PING, RTDBD_PROTO_VERSION - 1, nullptr, 0));

    /* 服务端应关闭连接：recv 返回 0（EOF） */
    char buf[64];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    EXPECT_EQ(n, 0) << "协议版本不匹配（v2 客户端）时服务端应关闭连接，不发响应";

    close(fd);
    p.stop();
}

/* 6. 策略分区限定：同 uid 在不同分区权限不同 */
TEST(ProtoV3, PolicyPartitionScoped)
{
    /* 策略：uid 仅允许分区 1 的 id 0..999 */
    RtdbdProc p;
    ASSERT_TRUE(p.start("policypart", policy_for_uid_partition((unsigned long)getuid(), 1), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 先 SELECT 到分区 1 */
    uint32_t sel_part = 1;
    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 分区 1 创建点位 → 成功 */
    rtdbd_create_req_t c1;
    memset(&c1, 0, sizeof(c1));
    c1.point_id = 20;
    c1.type     = RTDBD_TYPE_INT32;
    c1.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c1.name, "Part1.Point", sizeof(c1.name) - 1);
    c1.partition = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c1, sizeof(c1), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    /* 切到分区 2 尝试创建 → 被拒（DENIED） */
    sel_part = 2;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    rtdbd_create_req_t c2;
    memset(&c2, 0, sizeof(c2));
    c2.point_id = 21;
    c2.type     = RTDBD_TYPE_INT32;
    c2.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c2.name, "Part2.Point", sizeof(c2.name) - 1);
    c2.partition = 2;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c2, sizeof(c2), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    close(fd);
    p.stop();
}

/* 7. 审计记录包含 partition 字段 */
TEST(ProtoV3, AuditRecordsPartition)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("auditpart", policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    /* 创建分区 1 的点位 */
    rtdbd_create_req_t c;
    memset(&c, 0, sizeof(c));
    c.point_id = 20;
    c.type     = RTDBD_TYPE_INT32;
    c.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c.name, "Audit.Point", sizeof(c.name) - 1);
    c.partition = 1;

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c, sizeof(c), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* SELECT 到分区 1 并写入 */
    uint32_t sel_part = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id = 20;
    w.type = RTDBD_TYPE_INT32;
    w.value_bits = (uint64_t)(int32_t)12345;
    w.source_ts_ns = 0;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_WRITE, &w, sizeof(w), &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);

    /* 审计导出 */
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_AUDIT_DUMP, nullptr, 0, &resp, &body));
    ASSERT_EQ(resp.status, RTDBD_ST_OK);
    ASSERT_GE(body.size(), 2u * sizeof(rtdbd_audit_entry_t));  /* 至少 CREATE + WRITE 两条 */

    uint32_t n = (uint32_t)(body.size() / sizeof(rtdbd_audit_entry_t));
    bool found_create = false, found_write = false;
    for (uint32_t i = 0; i < n; ++i) {
        rtdbd_audit_entry_t ae;
        memcpy(&ae, body.data() + i * sizeof(ae), sizeof(ae));
        if (ae.point_id == 20 && ae.partition == 1) {
            if (i < n - 1) found_write = true;  /* 最后一条是 WRITE */
            else found_create = true;           /* 倒数第二条是 CREATE */
        }
    }
    EXPECT_TRUE(found_create) << "CREATE 审计记录应包含 partition=1";
    EXPECT_TRUE(found_write)  << "WRITE 审计记录应包含 partition=1";

    close(fd);
    p.stop();
}

/* 8. 分区白名单拒绝非法分区 */
TEST(ProtoV3, PartitionsWhitelistRejects)
{
    /* 配置白名单 [0, 1] */
    RtdbdProc p;
    ASSERT_TRUE(p.start("whitelist", policy_for_uid((unsigned long)getuid()), kConfigWithWhitelist));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    rtdbd_resp_hdr_t resp;
    std::vector<uint8_t> body;

    /* SELECT(2) → 分区 2 不在白名单 → DENIED */
    uint32_t sel_part = 2;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    /* 连接默认分区仍为 0（SELECT 失败不改状态） */
    sel_part = 0;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    /* CREATE_POINT 在分区 2 → DENIED */
    rtdbd_create_req_t c2;
    memset(&c2, 0, sizeof(c2));
    c2.point_id = 30;
    c2.type     = RTDBD_TYPE_INT32;
    c2.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c2.name, "BadPartition.Point", sizeof(c2.name) - 1);
    c2.partition = 2;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c2, sizeof(c2), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_DENIED);

    /* CREATE_POINT 在分区 1 → OK */
    rtdbd_create_req_t c1;
    memset(&c1, 0, sizeof(c1));
    c1.point_id = 31;
    c1.type     = RTDBD_TYPE_INT32;
    c1.access   = INDURTDB_ACCESS_READ_WRITE;
    strncpy(c1.name, "GoodPartition.Point", sizeof(c1.name) - 1);
    c1.partition = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_CREATE_POINT, &c1, sizeof(c1), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    /* SELECT(1) → OK */
    sel_part = 1;
    ASSERT_TRUE(raw_exchange(fd, RTDBD_OP_SELECT, &sel_part, sizeof(sel_part), &resp, &body));
    EXPECT_EQ(resp.status, RTDBD_ST_OK);

    close(fd);
    p.stop();
}

/* 8. BATCH_READ / BATCH_WRITE 1024 点：顺序、逐项状态与值保持。 */
TEST(ProtoV3, BatchReadWrite1024)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start_with("batch_1024",
                             policy_for_uid_range((unsigned long)getuid(), 2047),
                             kConfigBase, 2048, 10000));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    std::vector<uint32_t> ids;
    std::vector<rtdbd_write_req_t> writes;
    ids.reserve(1024);
    writes.reserve(1024);
    char name[32];
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t id = 100 + i;
        snprintf(name, sizeof(name), "Batch_%u", id);
        ASSERT_TRUE(create_point(fd, id, name)) << "create " << id;
        ids.push_back(id);
        writes.push_back(make_write(id, (int32_t)(id * 3)));
    }

    std::vector<rtdbd_batch_write_item_t> witems;
    uint16_t st = 0;
    ASSERT_TRUE(batch_write(fd, writes, &witems, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(witems.size(), 1024u);
    for (size_t i = 0; i < witems.size(); ++i) {
        EXPECT_EQ(witems[i].point_id, ids[i]);
        EXPECT_EQ(witems[i].status, RTDBD_ST_OK);
    }

    std::vector<rtdbd_batch_read_item_t> ritems;
    ASSERT_TRUE(batch_read(fd, ids, &ritems, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(ritems.size(), 1024u);
    for (size_t i = 0; i < ritems.size(); ++i) {
        EXPECT_EQ(ritems[i].point_id, ids[i]);
        EXPECT_EQ(ritems[i].status, RTDBD_ST_OK);
        EXPECT_EQ((int32_t)(int64_t)ritems[i].value.value_bits,
                  (int32_t)(ids[i] * 3));
    }

    close(fd);
}

/* 9. BATCH_READ 部分失败：有效/未注册混合，顺序不变，失败项值全 0。 */
TEST(ProtoV3, BatchReadPartialStatus)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_read_partial",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));

    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    std::vector<rtdbd_batch_read_item_t> items;
    uint16_t st = 0;
    ASSERT_TRUE(batch_read(fd, {10, 999, 11}, &items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].point_id, 10u);
    EXPECT_EQ(items[0].status, RTDBD_ST_OK);
    EXPECT_EQ(items[1].point_id, 999u);
    EXPECT_EQ(items[1].status, RTDBD_ST_NOT_FOUND);
    rtdbd_get_resp_t zero;
    memset(&zero, 0, sizeof(zero));
    EXPECT_EQ(memcmp(&items[1].value, &zero, sizeof(zero)), 0);
    EXPECT_EQ(items[2].point_id, 11u);
    EXPECT_EQ(items[2].status, RTDBD_ST_OK);

    close(fd);
}

/* 10. BATCH_READ 允许重复 id，且按请求顺序返回。 */
TEST(ProtoV3, BatchReadDuplicateIds)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_read_dup",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    std::vector<rtdbd_batch_read_item_t> items;
    uint16_t st = 0;
    ASSERT_TRUE(batch_read(fd, {10, 10}, &items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].point_id, 10u);
    EXPECT_EQ(items[1].point_id, 10u);
    EXPECT_EQ(items[0].status, RTDBD_ST_OK);
    EXPECT_EQ(items[1].status, RTDBD_ST_OK);

    close(fd);
}

/* 11. BATCH_READ 整包校验：count=0 / 超 max_batch / payload 长度不符 → BAD_REQUEST。 */
TEST(ProtoV3, BatchReadRejectsBadCount)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_read_bad",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));

    for (int kind = 0; kind < 3; ++kind) {
        int fd = connect_to(p.sock);
        ASSERT_GE(fd, 0);
        rtdbd_resp_hdr_t resp;
        std::vector<uint8_t> body;
        bool ok = false;

        if (kind == 0) {
            uint32_t count = 0;
            ok = raw_exchange(fd, RTDBD_OP_BATCH_READ, &count, sizeof(count),
                              &resp, &body);
        } else if (kind == 1) {
            std::vector<uint8_t> payload(4 + 1025 * 4);
            uint32_t count = 1025;
            memcpy(payload.data(), &count, 4);
            ok = raw_exchange(fd, RTDBD_OP_BATCH_READ, payload.data(),
                              (uint32_t)payload.size(), &resp, &body);
        } else {
            uint8_t payload[12];
            uint32_t count = 2, id = 10;
            memcpy(payload, &count, 4);
            memcpy(payload + 4, &id, 4);
            /* 只带 12B，但 count=2 要求 4+8=12？这里用 8B 让长度不符 */
            ok = raw_exchange(fd, RTDBD_OP_BATCH_READ, payload, 8, &resp, &body);
        }

        ASSERT_TRUE(ok);
        EXPECT_EQ(resp.status, RTDBD_ST_BAD_REQUEST) << "kind=" << kind;
        close(fd);
    }
}

/* 12. BATCH_WRITE 部分失败：成功项落库，失败项不影响其他项。 */
TEST(ProtoV3, BatchWritePartialFailure)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_write_partial",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(create_point(fd, 12, "Batch12"));

    std::vector<rtdbd_write_req_t> writes = {
        make_write(10, 111),
        make_write(999, 222),
        make_write(12, 333),
    };
    std::vector<rtdbd_batch_write_item_t> items;
    uint16_t st = 0;
    ASSERT_TRUE(batch_write(fd, writes, &items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(items.size(), 3u);
    EXPECT_EQ(items[0].status, RTDBD_ST_OK);
    EXPECT_EQ(items[1].status, RTDBD_ST_NOT_FOUND);
    EXPECT_EQ(items[2].status, RTDBD_ST_OK);

    std::vector<rtdbd_batch_read_item_t> read_items;
    ASSERT_TRUE(batch_read(fd, {10, 12}, &read_items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(read_items.size(), 2u);
    EXPECT_EQ((int32_t)(int64_t)read_items[0].value.value_bits, 111);
    EXPECT_EQ((int32_t)(int64_t)read_items[1].value.value_bits, 333);

    close(fd);
}

/* 13. BATCH_WRITE 允许重复 id，按请求顺序执行，后写覆盖前写。 */
TEST(ProtoV3, BatchWriteDuplicateIds)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_write_dup",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);

    std::vector<rtdbd_write_req_t> writes = {
        make_write(10, 1),
        make_write(10, 2),
    };
    std::vector<rtdbd_batch_write_item_t> items;
    uint16_t st = 0;
    ASSERT_TRUE(batch_write(fd, writes, &items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].status, RTDBD_ST_OK);
    EXPECT_EQ(items[1].status, RTDBD_ST_OK);

    std::vector<rtdbd_batch_read_item_t> read_items;
    ASSERT_TRUE(batch_read(fd, {10}, &read_items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    EXPECT_EQ((int32_t)(int64_t)read_items[0].value.value_bits, 2);

    close(fd);
}

/* 14. BATCH_WRITE 跨分区：逐项从点位元数据取 partition 鉴权。 */
TEST(ProtoV3, BatchWriteCrossPartition)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start_with("batch_write_cross",
                             policy_for_uid_range((unsigned long)getuid(), 63),
                             kConfigBase, 64, 4));
    int fd = connect_to(p.sock);
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(create_point(fd, 20, "BatchPart1", 1));

    std::vector<rtdbd_write_req_t> writes = {
        make_write(10, 10),
        make_write(20, 20),
    };
    std::vector<rtdbd_batch_write_item_t> items;
    uint16_t st = 0;
    ASSERT_TRUE(batch_write(fd, writes, &items, &st));
    ASSERT_EQ(st, RTDBD_ST_OK);
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].status, RTDBD_ST_OK);
    EXPECT_EQ(items[1].status, RTDBD_ST_OK);

    close(fd);
}

/* 15. BATCH_WRITE 整包校验：count=0 / 超 max_batch / payload 长度不符 → BAD_REQUEST。 */
TEST(ProtoV3, BatchWriteRejectsBadPayload)
{
    RtdbdProc p;
    ASSERT_TRUE(p.start("batch_write_bad",
                        policy_for_uid((unsigned long)getuid()), kConfigBase));

    for (int kind = 0; kind < 3; ++kind) {
        int fd = connect_to(p.sock);
        ASSERT_GE(fd, 0);
        rtdbd_resp_hdr_t resp;
        std::vector<uint8_t> body;
        bool ok = false;

        if (kind == 0) {
            uint32_t count = 0;
            ok = raw_exchange(fd, RTDBD_OP_BATCH_WRITE, &count, sizeof(count),
                              &resp, &body);
        } else if (kind == 1) {
            std::vector<uint8_t> payload(4 + 1025 * sizeof(rtdbd_write_req_t));
            uint32_t count = 1025;
            memcpy(payload.data(), &count, 4);
            ok = raw_exchange(fd, RTDBD_OP_BATCH_WRITE, payload.data(),
                              (uint32_t)payload.size(), &resp, &body);
        } else {
            uint32_t count = 1;
            ok = raw_exchange(fd, RTDBD_OP_BATCH_WRITE, &count, sizeof(count),
                              &resp, &body);  /* 缺少 24B item */
        }

        ASSERT_TRUE(ok);
        EXPECT_EQ(resp.status, RTDBD_ST_BAD_REQUEST) << "kind=" << kind;
        close(fd);
    }
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