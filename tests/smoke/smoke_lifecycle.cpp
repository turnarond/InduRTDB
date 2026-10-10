/**
 * @file smoke_lifecycle.cpp
 * @brief InduRTDB C++ API 全生命周期冒烟测试
 * @version 1.0.0
 * @date 2026-09-17
 * @copyright MIT License
 *
 * 冒烟测试覆盖：init → write(全部类型) → read → peek → subscribe →
 *             unsubscribe → heartbeat → get_write_count → is_initialized → shutdown
 *
 * 不依赖 GTest，独立可执行文件，返回 0 表示全部通过，非 0 表示失败。
 */

#include <indurtdb.hpp>

#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (condition) {
        std::printf("  [PASS] %s\n", msg);
    } else {
        std::printf("  [FAIL] %s\n", msg);
        ++g_failures;
    }
}

// 订阅回调：记录被调用次数与最后一次写入的值
int g_callback_count = 0;
double g_last_callback_value = 0.0;

void on_point_change(indurtdb::PointId id,
                     const indurtdb::PointData& data,
                     void* user_data) {
    (void)id;
    (void)user_data;
    ++g_callback_count;
    g_last_callback_value = data.value.d;
}

} // namespace

int main() {
    std::printf("=== InduRTDB C++ API 生命周期冒烟测试 ===\n\n");

    auto& rtdb = indurtdb::InduRTDB::instance();

    // ---------- 阶段 1: 初始化 ----------
    std::printf("[1] 初始化数据库\n");
    check(!rtdb.is_initialized(), "初始化前 is_initialized() == false");

    bool init_ok = rtdb.initialize("smoke_lifecycle", 4096, 16);
    check(init_ok, "initialize() 返回 true");
    check(rtdb.is_initialized(), "初始化后 is_initialized() == true");

    if (!init_ok) {
        std::printf("\n[FATAL] 初始化失败，冒烟测试中止\n");
        return 1;
    }

    // ---------- 阶段 2: 写入全部数据类型 ----------
    std::printf("\n[2] 写入数据 (double / int32 / bool / string)\n");
    check(rtdb.write(1001, 23.5),            "write double (1001, 23.5)");
    check(rtdb.write(1002, (int32_t)42),     "write int32  (1002, 42)");
    check(rtdb.write(1003, true),            "write bool   (1003, true)");
    check(rtdb.write(1004, "HVAC-01"),       "write string (1004, \"HVAC-01\")");

    // 非法点位写入应失败
    check(!rtdb.write(99999, 1.0),           "write 非法 ID 返回 false");

    // ---------- 阶段 3: 读取数据 ----------
    std::printf("\n[3] 读取数据 (read)\n");
    {
        indurtdb::PointData p;
        check(rtdb.read(1001, p), "read(1001) 返回 true");
        check(p.value.d == 23.5, "read(1001).value.d == 23.5");
        check(p.type == indurtdb::PointType::DOUBLE, "read(1001).type == DOUBLE");
        check(p.quality == indurtdb::Quality::GOOD, "read(1001).quality == GOOD");
        check(p.timestamp_ns > 0, "read(1001).timestamp_ns > 0");
    }
    {
        indurtdb::PointData p;
        check(rtdb.read(1002, p), "read(1002) 返回 true");
        check(p.value.i == 42,    "read(1002).value.i == 42");
    }
    {
        indurtdb::PointData p;
        check(rtdb.read(1003, p), "read(1003) 返回 true");
        check(p.value.b == true,  "read(1003).value.b == true");
    }
    {
        indurtdb::PointData p;
        check(rtdb.read(1004, p),             "read(1004) 返回 true");
        check(std::strcmp(p.value.str, "HVAC-01") == 0,
              "read(1004).value.str == \"HVAC-01\"");
    }
    {
        indurtdb::PointData p;
        check(!rtdb.read(99999, p), "read 未写入 ID 返回 false");
    }

    // ---------- 阶段 4: peek 零拷贝读取 ----------
    std::printf("\n[4] peek 零拷贝读取\n");
    {
        const indurtdb::PointData* pp = rtdb.peek(1001);
        check(pp != nullptr,         "peek(1001) 返回非空指针");
        check(pp->value.d == 23.5,   "peek(1001)->value.d == 23.5");
    }
    {
        const indurtdb::PointData* pp = rtdb.peek(99999);
        check(pp == nullptr, "peek 未写入 ID 返回空指针");
    }

    // ---------- 阶段 5: 订阅 / 取消订阅 ----------
    std::printf("\n[5] 订阅与回调触发\n");
    g_callback_count = 0;
    bool sub_ok = rtdb.subscribe(1001, on_point_change, nullptr);
    check(sub_ok, "subscribe(1001) 返回 true");

    // 再次写入触发回调
    check(rtdb.write(1001, 99.9), "write(1001, 99.9) 触发回调");
    check(g_callback_count == 1,  "回调被调用 1 次");
    check(g_last_callback_value == 99.9, "回调收到的值 == 99.9");

    check(rtdb.unsubscribe(1001), "unsubscribe(1001) 返回 true");

    // 取消订阅后再次写入不应触发回调
    int before = g_callback_count;
    rtdb.write(1001, 123.4);
    check(g_callback_count == before, "取消订阅后回调不再触发");

    // ---------- 阶段 6: 心跳与统计 ----------
    std::printf("\n[6] 心跳与写入统计\n");
    uint64_t count_before = rtdb.get_write_count();
    rtdb.update_heartbeat();
    check(rtdb.get_write_count() == count_before,
          "update_heartbeat() 不增加 write_count");

    rtdb.write(2001, 1.0);
    check(rtdb.get_write_count() == count_before + 1,
          "write 后 write_count +1");

    // ---------- 阶段 7: 关闭 ----------
    std::printf("\n[7] 关闭数据库\n");
    rtdb.shutdown();
    check(!rtdb.is_initialized(), "shutdown 后 is_initialized() == false");

    // ---------- 结果汇总 ----------
    std::printf("\n========================================\n");
    if (g_failures == 0) {
        std::printf("冒烟测试结果: 全部通过 (PASS)\n");
        std::printf("========================================\n");
        return 0;
    } else {
        std::printf("冒烟测试结果: %d 项失败 (FAIL)\n", g_failures);
        std::printf("========================================\n");
        return 1;
    }
}
