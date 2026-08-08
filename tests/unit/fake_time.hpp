/**
 * @file fake_time.hpp
 * @brief 测试用 FakeTime —— 可控的 osal::ITime 实现
 * @version 2.1.0
 *
 * 测试代码不受 Core 层"零虚函数"约束, 可自由继承 OSAL 接口。
 */

#pragma once

#include <indurtdb/osal/interface.hpp>

namespace indurtdb {
namespace test {

class FakeTime : public osal::ITime {
public:
    explicit FakeTime(TimestampNs now = 1000) : now_(now) {}

    TimestampNs now_ns() const override { return now_; }
    void sleep_ns(TimestampNs) const override {}

    void set_now(TimestampNs now) { now_ = now; }

private:
    TimestampNs now_;
};

} // namespace test
} // namespace indurtdb
