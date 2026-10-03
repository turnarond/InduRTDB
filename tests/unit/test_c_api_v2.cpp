/**
 * @file test_c_api_v2.cpp
 * @brief v3.4 T4 红阶段用例: API v2 句柄化 (去单例, 多实例并发)
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 *
 * 设计依据: docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md §3.4
 *   indurtdb_t* 不透明句柄; indurtdb_h_open / indurtdb_h_close;
 *   v1 的 26 个全局函数保留为薄封装 (默认句柄), 标注 deprecated, 不破既有调用方.
 *
 * 用例 (红阶段应全部失败: indurtdb_t / indurtdb_cfg_t / indurtdb_h_* 尚未实现):
 *   ApiV2.MultipleInstancesInOneProcess  同进程持两实例, 各自读写互不影响
 *   ApiV2.FindByNameIsPerInstance        两实例索引互相独立
 *   ApiV2.OpenRejectsBadArgs             空 out / 空名 / max_points=0 -> ERR_ARG
 *   ApiV2.CloseFreesSlot                 关闭后可再次 open (槽位回收)
 *   ApiV2.SingletonRegression            旧 v1 indurtdb_initialize 仍可用
 */

#include <gtest/gtest.h>

#include <string>

extern "C" {
#include <indurtdb/indurtdb.h>
}

namespace {

std::string uniq(const char* tag) {
    static int seq = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "apiv2_%s_%d_%d", tag, (int)getpid(), ++seq);
    return std::string(buf);
}

class ApiV2Test : public ::testing::Test {
protected:
    void TearDown() override { /* 清理可能残留的段 */ }
};

/* 1. 同进程两个实例, 各自读写互不影响 */
TEST_F(ApiV2Test, MultipleInstancesInOneProcess) {
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;

    indurtdb_t* a = nullptr;
    indurtdb_t* b = nullptr;
    ASSERT_EQ(indurtdb_h_open(&a, uniq("A").c_str(), &cfg), 0);
    ASSERT_EQ(indurtdb_h_open(&b, uniq("B").c_str(), &cfg), 0);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(a, b);

    /* A 写 id=1 = 100, B 写 id=1 = 200 */
    ASSERT_EQ(indurtdb_h_write_int32(a, 1, 100), 0);
    ASSERT_EQ(indurtdb_h_write_int32(b, 1, 200), 0);

    int32_t va = -1, vb = -1;
    ASSERT_EQ(indurtdb_h_read_int32(a, 1, &va), 0);
    ASSERT_EQ(indurtdb_h_read_int32(b, 1, &vb), 0);
    EXPECT_EQ(va, 100);
    EXPECT_EQ(vb, 200);

    /* A 再次读, 仍为自己的值 (B 的写入不污染 A) */
    ASSERT_EQ(indurtdb_h_read_int32(a, 1, &va), 0);
    EXPECT_EQ(va, 100);

    indurtdb_h_close(a);
    indurtdb_h_close(b);
}

/* 2. 索引按实例隔离: 同名 "sensor.x" 在两实例注册到不同 id, 各自只查到自己 */
TEST_F(ApiV2Test, FindByNameIsPerInstance) {
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64; cfg.max_subscribers = 4;

    indurtdb_t* a = nullptr, * b = nullptr;
    ASSERT_EQ(indurtdb_h_open(&a, uniq("A").c_str(), &cfg), 0);
    ASSERT_EQ(indurtdb_h_open(&b, uniq("B").c_str(), &cfg), 0);

    /* 给 A / B 各写配置文件: 同名 "sensor.x", 但 A 映射到 id=1, B 映射到 id=2 */
    const std::string ca = "/tmp/apiv2_A_" + uniq("cfg") + ".yaml";
    const std::string cb = "/tmp/apiv2_B_" + uniq("cfg") + ".yaml";
    {
        FILE* fa = fopen(ca.c_str(), "w"); ASSERT_NE(fa, nullptr);
        fprintf(fa, "points:\n  - id: 1\n    name: sensor.x\n    type: int32\n");
        fclose(fa);
        FILE* fb = fopen(cb.c_str(), "w"); ASSERT_NE(fb, nullptr);
        fprintf(fb, "points:\n  - id: 2\n    name: sensor.x\n    type: int32\n");
        fclose(fb);
    }
    ASSERT_EQ(indurtdb_h_load_config(a, ca.c_str()), 0);
    ASSERT_EQ(indurtdb_h_load_config(b, cb.c_str()), 0);

    uint32_t id = 0;
    ASSERT_EQ(indurtdb_h_find_by_name(a, "sensor.x", &id), 0);
    EXPECT_EQ(id, 1u) << "A 实例应解析到自己的 id=1";
    ASSERT_EQ(indurtdb_h_find_by_name(b, "sensor.x", &id), 0);
    EXPECT_EQ(id, 2u) << "B 实例应解析到自己的 id=2 (索引按实例隔离)";

    /* B 不认识 A 的私有名 */
    EXPECT_EQ(indurtdb_h_find_by_name(b, "only.in.a", &id), INDURTDB_ERR_NOT_FOUND);

    indurtdb_h_close(a);
    indurtdb_h_close(b);
    ::unlink(ca.c_str());
    ::unlink(cb.c_str());
}

/* 3. open 参数校验 */
TEST_F(ApiV2Test, OpenRejectsBadArgs) {
    indurtdb_cfg_t cfg{};
    cfg.max_points = 16; cfg.max_subscribers = 0;
    indurtdb_t* h = nullptr;

    EXPECT_EQ(indurtdb_h_open(nullptr, "x", &cfg), INDURTDB_ERR_ARG);
    EXPECT_EQ(indurtdb_h_open(&h, "", &cfg), INDURTDB_ERR_ARG);
    EXPECT_EQ(indurtdb_h_open(&h, "x", nullptr), INDURTDB_ERR_ARG);

    indurtdb_cfg_t zero{};
    zero.max_points = 0;
    EXPECT_EQ(indurtdb_h_open(&h, "x", &zero), INDURTDB_ERR_ARG);
}

/* 4. 关闭后槽位可回收: 再次 open 成功 */
TEST_F(ApiV2Test, CloseFreesSlot) {
    indurtdb_cfg_t cfg{};
    cfg.max_points = 16; cfg.max_subscribers = 0;

    indurtdb_t* h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, uniq("H").c_str(), &cfg), 0);
    indurtdb_h_close(h);
    indurtdb_h_close(h);   /* 双重关闭安全 (幂等) */

    indurtdb_t* h2 = nullptr;
    EXPECT_EQ(indurtdb_h_open(&h2, uniq("H2").c_str(), &cfg), 0);
    indurtdb_h_close(h2);
}

/* 5. 旧 v1 单例 API 仍可用 (回归) */
TEST_F(ApiV2Test, SingletonRegression) {
    const std::string inst = uniq("legacy");
    ASSERT_EQ(indurtdb_initialize(inst.c_str(), 32, 4), 0);
    ASSERT_EQ(indurtdb_write_int32(3, 42), 0);
    int32_t v = 0;
    ASSERT_EQ(indurtdb_read_int32(3, &v), 0);
    EXPECT_EQ(v, 42);
    indurtdb_shutdown();
}

}  // namespace
