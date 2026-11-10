/**
 * @file test_c_dispatch.cpp
 * @brief M1 双前端等价协议层单测：dispatch 等价性 / URL↔opcode 映射 /
 *       status 映射 / PING transport 无关确定性。
 *
 * 不依赖 VSOA SDK、不依赖 rtdbd.c 全局状态；直接测 rtdbd/src/dispatch 纯模块。
 */
#include <gtest/gtest.h>

#include "rtdbd_dispatch.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unistd.h>

TEST(Dispatch, StatusMapPreservesKnownCodes) {
    // doc 13 §7.2：rtdbd status 用 0x80 + RTDBD_ST_*，7 个状态码无损映射
    EXPECT_EQ(rtdbd_st_to_vsoa_status(0), 0x80);  // OK
    EXPECT_EQ(rtdbd_st_to_vsoa_status(1), 0x81);  // BAD_REQUEST
    EXPECT_EQ(rtdbd_st_to_vsoa_status(2), 0x82);  // DENIED
    EXPECT_EQ(rtdbd_st_to_vsoa_status(4), 0x84);  // NOT_FOUND
    EXPECT_EQ(rtdbd_st_to_vsoa_status(9), 0x89);  // NOT_IMPLEMENTED
}

TEST(Dispatch, UrlToOpcodeMirrors13Capabilities) {
    uint16_t op = 0;
    EXPECT_EQ(rtdbd_opcode_from_url("/ping", &op), 0);
    EXPECT_EQ(op, 1);
    EXPECT_EQ(rtdbd_opcode_from_url("/point/write", &op), 0);
    EXPECT_EQ(op, 2);
    EXPECT_EQ(rtdbd_opcode_from_url("/point/batch_write", &op), 0);
    EXPECT_EQ(op, 20);
    EXPECT_EQ(rtdbd_opcode_from_url("/point/get", &op), 0);
    EXPECT_EQ(op, 10);
    EXPECT_EQ(rtdbd_opcode_from_url("/subscription/add", &op), 0);
    EXPECT_EQ(op, 4);
    EXPECT_EQ(rtdbd_opcode_from_url("/subscription/remove", &op), 0);
    EXPECT_EQ(op, 5);
    EXPECT_EQ(rtdbd_opcode_from_url("/subscription/set", &op), 0);
    EXPECT_EQ(op, 18);
    EXPECT_EQ(rtdbd_opcode_from_url("/notify", &op), 0);
    EXPECT_EQ(op, 6);
    EXPECT_EQ(rtdbd_opcode_from_url("/snapshot_done", &op), 0);
    EXPECT_EQ(op, 22);
    EXPECT_EQ(rtdbd_opcode_from_url("/caps", &op), 0);
    EXPECT_EQ(op, 21);
    EXPECT_EQ(rtdbd_opcode_from_url("/select", &op), 0);
    EXPECT_EQ(op, 17);
    EXPECT_EQ(rtdbd_opcode_from_url("/health", &op), 0);
    EXPECT_EQ(op, 16);
    // 非镜像能力（UDS-only）不应被识别
    EXPECT_NE(rtdbd_opcode_from_url("/audit_dump", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/find_by_name", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/get_meta", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/set_meta", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/list", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/create_point", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/delete_point", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/rename_point", &op), 0);
    EXPECT_NE(rtdbd_opcode_from_url("/get_log", &op), 0);
    // 非法输入
    EXPECT_NE(rtdbd_opcode_from_url(nullptr, &op), 0);
}

TEST(Dispatch, OpcodeToUrlRoundtrip) {
    EXPECT_STREQ(rtdbd_url_from_opcode(1), "/ping");
    EXPECT_STREQ(rtdbd_url_from_opcode(2), "/point/write");
    EXPECT_STREQ(rtdbd_url_from_opcode(20), "/point/batch_write");
    EXPECT_STREQ(rtdbd_url_from_opcode(16), "/health");
    // 非镜像 opcode 返回 NULL
    EXPECT_EQ(rtdbd_url_from_opcode(3), nullptr);   // AUDIT_DUMP
    EXPECT_EQ(rtdbd_url_from_opcode(7), nullptr);   // FIND_BY_NAME
    EXPECT_EQ(rtdbd_url_from_opcode(15), nullptr);  // GET_LOG
}

TEST(Dispatch, PingIsTransportAgnosticAndDeterministic) {
    // doc 13 §7.4：同一 (opcode, req) 在「两个前端」下必须产生字节一致的响应。
    // 第一步 PING 无响应体，仅验证确定性 + 无 transport 耦合。
    uint8_t req[12] = {0};  // PING 不读请求体，仅占位
    uint8_t resp_a[64], resp_b[64];
    size_t len_a = 0, len_b = 0;
    uint16_t st_a = rtdbd_dispatch(1 /* PING */, req, sizeof(req),
                                   resp_a, sizeof(resp_a), &len_a);
    uint16_t st_b = rtdbd_dispatch(1 /* PING */, req, sizeof(req),
                                   resp_b, sizeof(resp_b), &len_b);
    EXPECT_EQ(st_a, 0 /* RTDBD_ST_OK */);
    EXPECT_EQ(st_b, 0);
    EXPECT_EQ(len_a, len_b);
    EXPECT_EQ(0, memcmp(resp_a, resp_b, len_a));
}

TEST(Dispatch, NormalizeUrlStripsVersionSegment) {
    // doc 13 §95：VSOA URL 带版本段，须归一为裸能力路径再匹配 opcode 表
    char buf[64];
    ASSERT_TRUE(rtdbd_normalize_url("/indurtdb/v1/ping", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "/ping");
    ASSERT_TRUE(rtdbd_normalize_url("/indurtdb/v1/point/write", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "/point/write");
    ASSERT_TRUE(rtdbd_normalize_url("/indurtdb/v2/snapshot_done", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "/snapshot_done");
    // 无版本段：原样
    ASSERT_TRUE(rtdbd_normalize_url("/ping", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "/ping");
    // 只有版本段、无能力路径 → 空
    ASSERT_TRUE(rtdbd_normalize_url("/indurtdb/v1", buf, sizeof(buf)));
    EXPECT_STREQ(buf, "");
    // 归一后仍能命中 opcode 表
    ASSERT_TRUE(rtdbd_normalize_url("/indurtdb/v1/ping", buf, sizeof(buf)));
    uint16_t op = 0;
    ASSERT_EQ(rtdbd_opcode_from_url(buf, &op), 0);
    EXPECT_EQ(op, 1);
    // 缓冲不足 / 非法入参
    EXPECT_FALSE(rtdbd_normalize_url("/indurtdb/v1/point/write", buf, 4));
    EXPECT_FALSE(rtdbd_normalize_url(nullptr, buf, sizeof(buf)));
}

TEST(Dispatch, CapsBodyIsTransportIndependent) {
    // 迁移范式验证：/caps 的业务语义是**纯计算**，只依赖配置快照与协议 POD，
    // 两前端（UDS/VSOA）调用同一函数 → 产出的 32B 响应体必然字节一致。
    rtdbd_dispatch_cfg_t cfg;
    cfg.max_clients = 128;
    cfg.initial_clients = 32;
    cfg.max_subs_per_conn = 10000;
    cfg.initial_subs_per_conn = 2048;
    cfg.max_batch = 1024;

    uint8_t buf[64];
    size_t len = 0;
    EXPECT_EQ(rtdbd_dispatch_caps(&cfg, buf, sizeof(buf), &len), 0u);
    EXPECT_EQ(len, sizeof(rtdbd_caps_t));

    rtdbd_caps_t caps;
    std::memset(&caps, 0, sizeof(caps));
    std::memcpy(&caps, buf, sizeof(caps));
    EXPECT_EQ(caps.proto_version, RTDBD_PROTO_VERSION);
    EXPECT_EQ(caps.max_clients, 128u);
    EXPECT_EQ(caps.initial_clients, 32u);
    EXPECT_EQ(caps.max_subs_per_conn, 10000u);
    EXPECT_EQ(caps.initial_subs_per_conn, 2048u);
    EXPECT_EQ(caps.max_batch, 1024u);
    EXPECT_EQ(caps.flags, static_cast<uint32_t>(RTDBD_CAPS_F_BATCH_READ |
                                                RTDBD_CAPS_F_BATCH_WRITE |
                                                RTDBD_CAPS_F_SNAPSHOT_DONE));
    // 确定性：同配置两次调用字节完全一致（等价性的必要条件）
    uint8_t buf2[64];
    size_t len2 = 0;
    EXPECT_EQ(rtdbd_dispatch_caps(&cfg, buf2, sizeof(buf2), &len2), 0u);
    EXPECT_EQ(len, len2);
    EXPECT_EQ(0, std::memcmp(buf, buf2, len));
    // 缓冲不足必须失败且不越界写
    size_t len3 = 0;
    EXPECT_NE(rtdbd_dispatch_caps(&cfg, buf, 4, &len3), 0u);
}

TEST(Dispatch, GetReadsPointThroughSharedSemantic) {
    // /point/get 的业务语义（读点位当前值 + 组装 64B POD）迁入 dispatch 后，
    // UDS 与 VSOA 两前端读**同一个**点位必须得到字节一致的响应体。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "dispatch_get_%d", static_cast<int>(getpid()));
    indurtdb_t *h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
    ASSERT_NE(h, nullptr);
    ASSERT_EQ(indurtdb_h_create_point(h, 1, "p1", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    ASSERT_EQ(indurtdb_h_write_int32(h, 1, 42), 0);

    uint8_t buf[128];
    size_t len = 0;
    EXPECT_EQ(rtdbd_dispatch_get(h, 1, buf, sizeof(buf), &len), 0u);
    EXPECT_EQ(len, sizeof(rtdbd_get_resp_t));

    rtdbd_get_resp_t r;
    std::memset(&r, 0, sizeof(r));
    std::memcpy(&r, buf, sizeof(r));
    EXPECT_EQ(r.point_id, 1u);
    EXPECT_EQ(r.type, static_cast<uint8_t>(INDURTDB_TYPE_INT32));
    int32_t v = 0;
    std::memcpy(&v, &r.value_bits, sizeof(v));
    EXPECT_EQ(v, 42);

    // 未注册点位 / 越界 id 一律「未找到」（与既有 UDS 语义一致）
    EXPECT_EQ(rtdbd_dispatch_get(h, 999, buf, sizeof(buf), &len), 4u);
    EXPECT_EQ(len, 0u);
    // 缓冲不足不得越界写
    size_t len2 = 0;
    EXPECT_EQ(rtdbd_dispatch_get(h, 1, buf, 4, &len2), 5u);

    indurtdb_h_close(h);
}

TEST(Dispatch, WriteAuthzFollowsSharedPolicyAndConf) {
    // 写路径的共享语义：鉴权结论必须与**策略函数本身**一致（deny-by-default，
    // §8.3-4），且不允许出现「dispatch 自行其是」的判定。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "dispatch_write_%d", static_cast<int>(getpid()));
    indurtdb_t *h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(h, 1, "p1", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    irt_policy_t policy;
    irt_policy_init(&policy);
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf);

    rtdbd_write_req_t w;
    std::memset(&w, 0, sizeof(w));
    w.point_id = 1;
    w.type = RTDBD_TYPE_INT32;
    int32_t v = 42;
    std::memcpy(&w.value_bits, &v, sizeof(v));

    const uint32_t uid = 1000;
    const uint32_t partition = 0;
    indurtdb_meta_t meta;
    std::memset(&meta, 0, sizeof(meta));

    const int allowed = irt_policy_allows(&policy, uid, partition, 1);
    EXPECT_EQ(allowed, 0) << "空策略应 deny-by-default（§8.3-4）";

    /* 加载允许型策略：覆盖**成功写入**路径（否则成功路径只能靠集成测试兜底） */
    const char *policy_path = "/tmp/indurtdb_dispatch_write_policy.txt";
    {
        FILE *f = std::fopen(policy_path, "w");
        ASSERT_NE(f, nullptr);
        std::fputs("1000:*:0:999\n", f);
        std::fclose(f);
    }
    irt_policy_t allow_policy;
    irt_policy_init(&allow_policy);
    uint32_t pline = 0;
    ASSERT_EQ(irt_policy_load(&allow_policy, policy_path, &pline), 0);
    ASSERT_EQ(irt_policy_allows(&allow_policy, uid, partition, 1), 1);
    {
        const uint16_t st2 = rtdbd_dispatch_write(h, &allow_policy, &conf, &w,
                                                   uid, partition, &meta);
        EXPECT_EQ(st2, RTDBD_ST_OK);
        /* 值必须真的落库（读回验证） */
        uint8_t buf2[128];
        size_t len2 = 0;
        ASSERT_EQ(rtdbd_dispatch_get(h, 1, buf2, sizeof(buf2), &len2), 0u);
        rtdbd_get_resp_t r2;
        std::memset(&r2, 0, sizeof(r2));
        std::memcpy(&r2, buf2, sizeof(r2));
        int32_t got2 = 0;
        std::memcpy(&got2, &r2.value_bits, sizeof(got2));
        EXPECT_EQ(got2, 42);
        /* 不支持的类型 → BAD_REQUEST */
        rtdbd_write_req_t bad2 = w;
        bad2.type = RTDBD_TYPE_STRING;
        EXPECT_EQ(rtdbd_dispatch_write(h, &allow_policy, &conf, &bad2,
                                       uid, partition, &meta),
                  RTDBD_ST_BAD_REQUEST);
    }
    const uint16_t st = rtdbd_dispatch_write(h, &policy, &conf, &w, uid, partition, &meta);
    if (!allowed) {
        EXPECT_EQ(st, RTDBD_ST_DENIED);
    } else {
        EXPECT_NE(st, RTDBD_ST_DENIED);
        /* 成功时值必须真的落库（可读回） */
        uint8_t buf[128];
        size_t len = 0;
        ASSERT_EQ(rtdbd_dispatch_get(h, 1, buf, sizeof(buf), &len), 0u);
        rtdbd_get_resp_t r;
        std::memset(&r, 0, sizeof(r));
        std::memcpy(&r, buf, sizeof(r));
        int32_t got = 0;
        std::memcpy(&got, &r.value_bits, sizeof(got));
        EXPECT_EQ(got, 42);
    }

    /* 参数校验：缺任一必需依赖都必须被拒（不得默认放行） */
    EXPECT_EQ(rtdbd_dispatch_write(nullptr, &policy, &conf, &w, uid, partition, &meta),
              RTDBD_ST_BAD_REQUEST);
    EXPECT_EQ(rtdbd_dispatch_write(h, nullptr, &conf, &w, uid, partition, &meta),
              RTDBD_ST_BAD_REQUEST);
    EXPECT_EQ(rtdbd_dispatch_write(h, &policy, nullptr, &w, uid, partition, &meta),
              RTDBD_ST_BAD_REQUEST);
    EXPECT_EQ(rtdbd_dispatch_write(h, &policy, &conf, nullptr, uid, partition, &meta),
              RTDBD_ST_BAD_REQUEST);

    /* 不支持的类型 → BAD_REQUEST（须先过鉴权） */
    if (allowed) {
        rtdbd_write_req_t bad = w;
        bad.type = RTDBD_TYPE_STRING;
        EXPECT_EQ(rtdbd_dispatch_write(h, &policy, &conf, &bad, uid, partition, &meta),
                  RTDBD_ST_BAD_REQUEST);
    }

    indurtdb_h_close(h);
}

TEST(Dispatch, NotifyFrameAndRbeGateAreSharedSemantics) {
    // 通知帧组装 + RBE 门控是**共享语义**：UDS 走出站队列、VSOA 走定向帧，
    // 但帧内容与死区抑制状态机必须两端一致（§8.3-5）。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "dispatch_notify_%d", static_cast<int>(getpid()));
    indurtdb_t *h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(h, 1, "p1", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    rtdbd_write_req_t w;
    std::memset(&w, 0, sizeof(w));
    w.point_id = 1;
    w.type = RTDBD_TYPE_INT32;
    int32_t v = 42;
    std::memcpy(&w.value_bits, &v, sizeof(v));
    w.source_ts_ns = 123456789ull;

    rtdbd_notify_t nt;
    std::memset(&nt, 0, sizeof(nt));
    ASSERT_EQ(rtdbd_dispatch_notify(h, &w, &nt), 0u);
    EXPECT_EQ(nt.point_id, 1u);
    EXPECT_EQ(nt.type, static_cast<uint8_t>(RTDBD_TYPE_INT32));
    int32_t got = 0;
    std::memcpy(&got, &nt.value_bits, sizeof(got));
    EXPECT_EQ(got, 42);
    EXPECT_EQ(nt.source_ts_ns, 123456789ull);
    /* 确定性：两次组装字节一致 */
    rtdbd_notify_t nt2;
    std::memset(&nt2, 0, sizeof(nt2));
    ASSERT_EQ(rtdbd_dispatch_notify(h, &w, &nt2), 0u);
    EXPECT_EQ(0, std::memcmp(&nt, &nt2, sizeof(nt)));

    /* RBE 门控：未启用死区 → 首值必发、其后每次都发（与既有 UDS 语义一致） */
    rtdbd_subtab_t sub;
    ASSERT_TRUE(rtdbd_subtab_init(&sub, RTDBD_SUBTAB_MIN_CAP));
    indurtdb_meta_t meta;
    std::memset(&meta, 0, sizeof(meta));
    EXPECT_TRUE(rtdbd_dispatch_notify_rbe(&sub, &w, &meta));
    EXPECT_TRUE(rtdbd_dispatch_notify_rbe(&sub, &w, &meta));
    rtdbd_subtab_free(&sub);

    indurtdb_h_close(h);
}

TEST(Dispatch, UnknownOpcodesNotImplemented) {
    uint8_t resp[64];
    size_t len = 0;
    // AUDIT_DUMP(3) 等非镜像能力在 dispatch 层未实现
    EXPECT_EQ(rtdbd_dispatch(3, nullptr, 0, resp, sizeof(resp), &len), 9);
    // 尚未迁移的镜像能力返回 NOT_IMPLEMENTED（占位，后续里程碑落地）
    EXPECT_EQ(rtdbd_dispatch(2 /* WRITE */, nullptr, 0, resp, sizeof(resp), &len), 9);
    EXPECT_EQ(rtdbd_dispatch(10 /* GET */, nullptr, 0, resp, sizeof(resp), &len), 9);
}
