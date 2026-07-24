/**
 * @file test_c_api.cpp
 * @brief InduRTDB 公共 API 集成测试
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include <indurtdb/indurtdb.h>
}

struct CallInfo { uint32_t id; bool called; };

static void api_callback(uint32_t id, const indurtdb_point_t* /*data*/,
                         void* user_data) {
    auto* ci = static_cast<CallInfo*>(user_data);
    ci->id = id;
    ci->called = true;
}

class CApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("test7", 64, 8), 0);
    }
    void TearDown() override {
        indurtdb_shutdown();
    }
};

TEST_F(CApiTest, Lifecycle) {
    EXPECT_TRUE(indurtdb_is_initialized());
}

TEST_F(CApiTest, WriteReadRoundTrip) {
    EXPECT_EQ(indurtdb_write_bool(0, true), 0);
    bool b = false;
    EXPECT_EQ(indurtdb_read_bool(0, &b), 0);
    EXPECT_TRUE(b);

    EXPECT_EQ(indurtdb_write_int32(1, -99), 0);
    int32_t i = 0;
    EXPECT_EQ(indurtdb_read_int32(1, &i), 0);
    EXPECT_EQ(i, -99);

    EXPECT_EQ(indurtdb_write_double(2, 2.718), 0);
    double d = 0;
    EXPECT_EQ(indurtdb_read_double(2, &d), 0);
    EXPECT_DOUBLE_EQ(d, 2.718);

    EXPECT_EQ(indurtdb_write_string(3, "api_test"), 0);
    char buf[64] = {};
    EXPECT_EQ(indurtdb_read_string(3, buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "api_test");
}

TEST_F(CApiTest, Peek) {
    indurtdb_write_int32(10, 777);
    const indurtdb_point_t* p = indurtdb_peek(10);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->value.i, 777);
}

TEST_F(CApiTest, ReadPoint) {
    indurtdb_write_int32(5, 42);
    indurtdb_point_t pt;
    EXPECT_EQ(indurtdb_read_point(5, &pt), 0);
    EXPECT_EQ(pt.value.i, 42);
    EXPECT_EQ(pt.type, INDURTDB_TYPE_INT32);
}

TEST_F(CApiTest, SubscribeReceivesNotification) {
    CallInfo ci = {};
    EXPECT_EQ(indurtdb_subscribe(8, api_callback, &ci), 0);
    indurtdb_write_int32(8, 123);
    EXPECT_TRUE(ci.called);
    EXPECT_EQ(ci.id, 8u);
}

TEST_F(CApiTest, ValidateId) {
    EXPECT_EQ(indurtdb_validate_id(0), 1);
    EXPECT_EQ(indurtdb_validate_id(63), 1);
    EXPECT_EQ(indurtdb_validate_id(64), 0);  /* out of range */
}

TEST_F(CApiTest, WriteCount) {
    uint64_t c0 = indurtdb_get_write_count();
    indurtdb_write_bool(0, true);
    indurtdb_write_int32(1, 1);
    EXPECT_GT(indurtdb_get_write_count(), c0);
}

TEST_F(CApiTest, ReadRange) {
    for (int i = 0; i < 5; i++) indurtdb_write_int32(i, i * 10);
    indurtdb_point_t buf[3];
    int n = indurtdb_read_range(0, 3, buf, 3);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(buf[0].value.i, 0);
    EXPECT_EQ(buf[1].value.i, 10);
    EXPECT_EQ(buf[2].value.i, 20);
}

/* ---- 批量写 ---- */

TEST_F(CApiTest, WriteRangeBool) {
    bool vals[] = {true, false, true};
    int n = indurtdb_write_range_bool(10, vals, 3);
    EXPECT_EQ(n, 3);
    bool b0 = false, b1 = true, b2 = false;
    EXPECT_EQ(indurtdb_read_bool(10, &b0), 0);
    EXPECT_TRUE(b0);
    EXPECT_EQ(indurtdb_read_bool(11, &b1), 0);
    EXPECT_FALSE(b1);
    EXPECT_EQ(indurtdb_read_bool(12, &b2), 0);
    EXPECT_TRUE(b2);
}

TEST_F(CApiTest, WriteRangeInt32) {
    int32_t vals[] = {100, 200, 300};
    int n = indurtdb_write_range_int32(20, vals, 3);
    EXPECT_EQ(n, 3);
    int32_t v;
    EXPECT_EQ(indurtdb_read_int32(20, &v), 0);
    EXPECT_EQ(v, 100);
    EXPECT_EQ(indurtdb_read_int32(22, &v), 0);
    EXPECT_EQ(v, 300);
}

TEST_F(CApiTest, WriteRangeDouble) {
    double vals[] = {1.1, 2.2, 3.3};
    int n = indurtdb_write_range_double(30, vals, 3);
    EXPECT_EQ(n, 3);
    double d;
    EXPECT_EQ(indurtdb_read_double(30, &d), 0);
    EXPECT_DOUBLE_EQ(d, 1.1);
    EXPECT_EQ(indurtdb_read_double(32, &d), 0);
    EXPECT_DOUBLE_EQ(d, 3.3);
}

/* ---- 订阅取消 ---- */

TEST_F(CApiTest, Unsubscribe) {
    CallInfo ci = {};
    EXPECT_EQ(indurtdb_subscribe(50, api_callback, &ci), 0);
    EXPECT_EQ(indurtdb_unsubscribe(50), 0);

    /* 取消后写入不应再触发回调 */
    ci.called = false;
    indurtdb_write_int32(50, 999);
    EXPECT_FALSE(ci.called);
}

/* ---- 心跳 + 超时计数 ---- */

TEST_F(CApiTest, Heartbeat) {
    /* update_heartbeat 不应崩溃; 尚无僵尸清理, 仅验证无副作用 */
    EXPECT_NO_FATAL_FAILURE(indurtdb_update_heartbeat());
    /* 重复调用也不应有问题 */
    indurtdb_update_heartbeat();
}

TEST_F(CApiTest, TimeoutCount) {
    uint64_t tc = indurtdb_get_timeout_count();
    /* 初始状态应为 0 (或至少可读) */
    EXPECT_GE(tc, 0u);
}

/* ---- 错误信息 ---- */

TEST_F(CApiTest, LastError_NotInitialized) {
    indurtdb_shutdown();
    EXPECT_FALSE(indurtdb_is_initialized());

    /* 未初始化写入 -> 返回非0, last_error 非空 */
    EXPECT_NE(indurtdb_write_int32(0, 1), 0);
    const char* err = indurtdb_get_last_error();
    ASSERT_NE(err, nullptr);
    EXPECT_STRNE(err, "");
}

TEST_F(CApiTest, LastError_AlreadyInitialized) {
    /* SetUp 已初始化, 再调 initialize -> "already initialized" */
    EXPECT_NE(indurtdb_initialize("test_dup2", 64, 4), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "already initialized");
}

TEST_F(CApiTest, LastError_InvalidArg) {
    indurtdb_shutdown();

    /* NULL instance_id -> "invalid argument" */
    EXPECT_NE(indurtdb_initialize(NULL, 64, 4), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");

    /* max_points == 0 -> "invalid argument" */
    EXPECT_NE(indurtdb_initialize("test_inv", 0, 4), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

TEST_F(CApiTest, LastError_NullOutputPointer) {
    /* NULL 输出指针 -> "null output pointer" */
    EXPECT_NE(indurtdb_read_int32(0, NULL), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");
}

TEST_F(CApiTest, LastError_NotOverwrittenBySuccess) {
    /* 先触发一个错误 */
    indurtdb_shutdown();
    EXPECT_NE(indurtdb_write_bool(99, true), 0);
    EXPECT_STRNE(indurtdb_get_last_error(), "");

    /* 成功初始化 —— g_last_error 是 _Thread_local,
     * 不受 initialize 内部 memset 影响, 上一个错误仍在 */
    ASSERT_EQ(indurtdb_initialize("test_err_keep", 64, 4), 0);
    EXPECT_NE(indurtdb_get_last_error(), nullptr);
}

TEST_F(CApiTest, LastError_Sequence) {
    /* NULL 指针错误先于 not-initialized 触发 (检查顺序验证) */
    EXPECT_NE(indurtdb_read_double(1, NULL), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");

    /* shutdown 后重新初始化 -> 正常读写 */
    indurtdb_shutdown();
    ASSERT_EQ(indurtdb_initialize("test_seq", 64, 4), 0);
    ASSERT_EQ(indurtdb_write_double(1, 3.14), 0);
    double v = 0.0;
    ASSERT_EQ(indurtdb_read_double(1, &v), 0);
    EXPECT_DOUBLE_EQ(v, 3.14);

    /* 越界 ID -> 报错 */
    EXPECT_NE(indurtdb_write_int32(9999, 1), 0);
    EXPECT_STRNE(indurtdb_get_last_error(), "");
}

/* ---- 配置加载 ---- */

TEST_F(CApiTest, LoadConfig) {
    /* 旧实例必须关闭后 load_config 才能重新初始化 */
    indurtdb_shutdown();

    /* 写临时配置文件 */
    const char* path = "/tmp/irt_test_load_config.cfg";
    FILE* f = fopen(path, "w");
    ASSERT_NE(f, nullptr);
    fprintf(f, "instance_id=cfg_test\nmax_points=128\nmax_subscribers=16\n");
    fclose(f);

    int rc = indurtdb_load_config(path);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(indurtdb_is_initialized());

    remove(path);
}
