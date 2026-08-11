/**
 * @file test_c_config.cpp
 * @brief 配置加载器单元测试
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>

extern "C" {
#include "core/irt_config.h"
}

TEST(CConfig, Defaults) {
    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    EXPECT_STREQ(cfg.instance_id, "default");
    EXPECT_EQ(cfg.max_points, 10000u);
    EXPECT_EQ(cfg.max_subscribers, 32u);
}

TEST(CConfig, LoadFileRoundTrip) {
    const char* path = "/tmp/irt_test_config.txt";
    FILE* f = fopen(path, "w");
    ASSERT_NE(f, nullptr);
    fprintf(f, "# InduRTDB config\n");
    fprintf(f, "instance_id=test_hvac\n");
    fprintf(f, "max_points=5000\n");
    fprintf(f, "max_subscribers=16\n");
    fprintf(f, "\n");  /* 空行 */
    fprintf(f, "# trailing comment\n");
    fclose(f);

    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    int rc = irt_config_load_file(&cfg, path);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(cfg.instance_id, "test_hvac");
    EXPECT_EQ(cfg.max_points, 5000u);
    EXPECT_EQ(cfg.max_subscribers, 16u);
    remove(path);
}

TEST(CConfig, LoadFileMissingUsesDefaults) {
    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    int rc = irt_config_load_file(&cfg, "/tmp/irt_nonexistent_xyz.txt");
    EXPECT_EQ(rc, -1);
    /* 失败不改写已有值 */
    EXPECT_STREQ(cfg.instance_id, "default");
    EXPECT_EQ(cfg.max_points, 10000u);
}
