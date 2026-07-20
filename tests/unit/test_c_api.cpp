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
