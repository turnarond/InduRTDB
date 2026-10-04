/**
 * @file test_c_diag.cpp
 * @brief T8: 诊断与工具集集成测试（红→绿）
 *
 *  - Tools.InspectReportsHealth：进程内初始化段，irt-diag inspect 输出
 *    版本/点数/CRC/owner 且 crc_ok=true。
 *  - Tools.SmokeEndToEnd：irt-diag smoke 驱动 C harness 完成
 *    init→write→read→subscribe→shutdown，退出码 0。
 *  - Tools.DetectsFdLeak：irt-diag leak 驱动 harness 反复启停，断言无 fd 增长。
 *  - Coverage.TargetBuildsAndRuns：INDURTDB_ENABLE_COVERAGE=ON 配置+构建库通过，
 *    并产出 .gcno 插桩文件。
 */
#include <indurtdb/indurtdb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gtest/gtest.h"

#ifndef INDURTDB_TOOLS_DIR
#define INDURTDB_TOOLS_DIR ""
#endif
#ifndef INDURTDB_DIAG_HARNESS
#define INDURTDB_DIAG_HARNESS "irt_diag_harness"
#endif
#ifndef INDURTDB_SOURCE_DIR
#define INDURTDB_SOURCE_DIR "."
#endif

static void remove_segment(const std::string& id) {
    std::string path = "/dev/shm/indurtdb_" + id;
    ::unlink(path.c_str());
}

/* 运行命令并捕获 stdout 与子进程退出码（rc 从 `__RC__=<rc>` 解析）。 */
static int sh_capture(const std::string& cmd, std::string& out) {
    std::string full = cmd + " ; rc=$? ; echo __RC__=$rc";
    FILE* f = ::popen(full.c_str(), "r");
    out.clear();
    if (!f) return -1;
    char buf[1024];
    int rc = -1;
    while (::fgets(buf, sizeof(buf), f)) {
        std::string line(buf);
        if (line.find("__RC__=") != std::string::npos) {
            int v = -1;
            if (::sscanf(line.c_str(), "__RC__=%d", &v) == 1) rc = v;
            continue;
        }
        out += line;
    }
    ::pclose(f);
    return rc;
}

TEST(Tools, InspectReportsHealth) {
    const std::string id = "diag_health";
    remove_segment(id);
    ASSERT_EQ(indurtdb_initialize(id.c_str(), 64, 4), 0);
    ASSERT_EQ(indurtdb_write_int32(1, 7), 0);

    std::string cmd = std::string("cd \"") + INDURTDB_TOOLS_DIR
                      + "/irt_diag\" && python3 -m irt_diag.cli inspect --id " + id;
    std::string out;
    int rc = sh_capture(cmd, out);
    EXPECT_EQ(rc, 0) << "inspect 应返回 0(健康)\n" << out;
    EXPECT_NE(out.find("\"version\": 2"), std::string::npos)
        << "inspect 应输出 version=2\n" << out;
    EXPECT_NE(out.find("\"crc_ok\": true"), std::string::npos)
        << "inspect 应报告 crc_ok=true\n" << out;
    EXPECT_NE(out.find("\"owner_pid\""), std::string::npos)
        << "inspect 应报告 owner_pid\n" << out;
    EXPECT_NE(out.find("\"max_points\": 64"), std::string::npos)
        << "inspect 应报告 max_points\n" << out;

    indurtdb_shutdown();
    remove_segment(id);
}

TEST(Tools, SmokeEndToEnd) {
    const std::string id = "diag_smoke";
    remove_segment(id);
    std::string cmd = std::string("cd \"") + INDURTDB_TOOLS_DIR
                      + "/irt_diag\" && python3 -m irt_diag.cli smoke"
                        " --harness \"" INDURTDB_DIAG_HARNESS "\""
                        " --id " + id;
    std::string out;
    int rc = sh_capture(cmd, out);
    EXPECT_EQ(rc, 0) << "smoke 应退出码 0\n" << out;
    EXPECT_NE(out.find("SMOKE_INIT ok"), std::string::npos) << out;
    EXPECT_NE(out.find("SMOKE_WRITE ok"), std::string::npos) << out;
    EXPECT_NE(out.find("SMOKE_READ ok"), std::string::npos) << out;
    EXPECT_NE(out.find("SMOKE_SUB ok"), std::string::npos) << out;
    EXPECT_NE(out.find("SMOKE_SHUTDOWN ok"), std::string::npos) << out;
    remove_segment(id);
}

TEST(Tools, DetectsFdLeak) {
    const std::string id = "diag_leak";
    remove_segment(id);
    std::string cmd = std::string("cd \"") + INDURTDB_TOOLS_DIR
                      + "/irt_diag\" && python3 -m irt_diag.cli leak"
                        " --harness \"" INDURTDB_DIAG_HARNESS "\""
                        " --id " + id + " --cycles 20";
    std::string out;
    int rc = sh_capture(cmd, out);
    EXPECT_NE(out.find("LEAK_INIT_FD"), std::string::npos) << out;
    EXPECT_NE(out.find("LEAK_FINAL_FD"), std::string::npos) << out;
    EXPECT_EQ(rc, 0) << "fd 泄漏检测应报告无增长(rc=0)\n" << out;
    remove_segment(id);
}

TEST(Coverage, TargetBuildsAndRuns) {
    char tmpl[] = "/tmp/irt_cov_XXXXXX";
    char* dir = ::mkdtemp(tmpl);
    ASSERT_NE(dir, nullptr) << "mkdtemp 失败";

    std::string cfg = std::string("cmake -S \"") + INDURTDB_SOURCE_DIR
                      + "\" -B \"" + dir + "\""
                      + " -DINDURTDB_ENABLE_COVERAGE=ON"
                      + " -DINDURTDB_BUILD_TESTS=OFF"
                      + " -DINDURTDB_BUILD_EXAMPLES=OFF"
                      + " -DINDURTDB_BUILD_RTDBD=OFF"
                      + " -DINDURTDB_BUILD_CLIENT=OFF"
                      + " -DINDURTDB_BUILD_DIAG=OFF"
                      + " >/dev/null 2>&1";
    int rc = ::system(cfg.c_str());
    ASSERT_EQ(WEXITSTATUS(rc), 0) << "coverage 配置失败";

    std::string build = std::string("cmake --build \"") + dir
                        + "\" --target indurtdb -j4 >/dev/null 2>&1";
    rc = ::system(build.c_str());
    ASSERT_EQ(WEXITSTATUS(rc), 0) << "coverage 构建库失败";

    std::string check = std::string("test -n \"$(find \"") + dir
                        + "\" -name '*.gcno')\"";
    rc = ::system(check.c_str());
    EXPECT_EQ(WEXITSTATUS(rc), 0) << "未产出 .gcno（覆盖率插桩未生效）";

    std::string clean = std::string("rm -rf \"") + dir + "\"";
    (void)::system(clean.c_str());
}
