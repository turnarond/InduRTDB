/**
 * @file test_c_session.cpp
 * @brief M1.1 单测：传输无关会话（rtdbd_session_t）与延迟动作队列。
 *
 * 覆盖 §8.3-3 语义：会话字段传输无关、订阅+RBE 复用 subtab、
 * 延迟动作队列 FIFO / 容量拒绝 / 延迟而非立即执行。
 */
#include <gtest/gtest.h>

#include "rtdbd_session.h"

#include <cstring>

namespace {
struct MockSink {
    int sends;
    int closes;
    int last_len;
};
bool mock_send(void *owner, const void *frame, size_t len) {
    MockSink *m = static_cast<MockSink *>(owner);
    m->sends++;
    m->last_len = static_cast<int>(len);
    return true;
}
void mock_close(void *owner) {
    static_cast<MockSink *>(owner)->closes++;
}
} // namespace

TEST(Session, InitDefaultsAreTransportAgnostic) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    EXPECT_EQ(s.partition, 0u);
    EXPECT_FALSE(s.caps_negotiated);
    EXPECT_FALSE(s.snap_active);
    EXPECT_EQ(s.epoch, 0u);         // UDS 侧不参与纪元隔离
    EXPECT_STREQ(s.service_id, ""); // VSOA 握手前为空
    EXPECT_EQ(s.role, RTDBD_ROLE_UNKNOWN);
    EXPECT_EQ(s.sink.send, nullptr);
    rtdbd_session_free(&s);
}

TEST(Session, SubscriptionsAndRbeReuseSubtab) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    EXPECT_FALSE(rtdbd_subtab_has(&s.sub, 42));
    ASSERT_TRUE(rtdbd_subtab_insert(&s.sub, 42));
    EXPECT_TRUE(rtdbd_subtab_has(&s.sub, 42));
    EXPECT_TRUE(rtdbd_subtab_decide(&s.sub, 42, 1.0, nullptr)); // 首值必发
    rtdbd_subtab_remove(&s.sub, 42);
    EXPECT_FALSE(rtdbd_subtab_has(&s.sub, 42));
    rtdbd_session_free(&s);
}

TEST(Session, ServiceIdRejectsTooLongWithoutTruncating) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    EXPECT_TRUE(rtdbd_session_set_service_id(&s, "plc-line-3"));
    EXPECT_STREQ(s.service_id, "plc-line-3");
    char too_long[200];
    std::memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    EXPECT_FALSE(rtdbd_session_set_service_id(&s, too_long));
    EXPECT_STREQ(s.service_id, "plc-line-3");
    rtdbd_session_free(&s);
}

TEST(Session, SinkIsPlainFunctionPointerTable) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    MockSink m{0, 0, 0};
    s.sink.send = mock_send;
    s.sink.close = mock_close;
    s.sink.owner = &m;
    uint8_t frame[4] = {1, 2, 3, 4};
    EXPECT_TRUE(s.sink.send(s.sink.owner, frame, sizeof(frame)));
    EXPECT_EQ(m.sends, 1);
    EXPECT_EQ(m.last_len, 4);
    s.sink.close(s.sink.owner);
    EXPECT_EQ(m.closes, 1);
    rtdbd_session_free(&s);
}

TEST(Session, ResetClearsStateButKeepsSubscriptionCapacity) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    ASSERT_TRUE(rtdbd_subtab_insert(&s.sub, 7));
    s.partition = 5;
    s.epoch = 42;
    s.caps_negotiated = true;
    rtdbd_session_reset(&s);
    EXPECT_EQ(s.partition, 0u);
    EXPECT_EQ(s.epoch, 0u);
    EXPECT_FALSE(s.caps_negotiated);
    EXPECT_FALSE(rtdbd_subtab_has(&s.sub, 7));
    EXPECT_TRUE(rtdbd_subtab_insert(&s.sub, 8)); // 容量保留，订阅表仍可用
    rtdbd_session_free(&s);
}

TEST(Session, SubscriptionsAreOrderedAndConsistent) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    EXPECT_EQ(rtdbd_session_subs_count(&s), 0u);
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));

    // 新增：有序视图与合并表同步
    EXPECT_EQ(rtdbd_session_subs_add(&s, 11, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    EXPECT_EQ(rtdbd_session_subs_add(&s, 22, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    EXPECT_EQ(rtdbd_session_subs_count(&s), 2u);
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 11));
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 22));
    EXPECT_FALSE(rtdbd_session_subs_contains(&s, 33));
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));

    // 幂等：已订阅不是错误（与既有 UDS 行为一致）
    EXPECT_EQ(rtdbd_session_subs_add(&s, 11, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_EXISTS);
    EXPECT_EQ(rtdbd_session_subs_count(&s), 2u);

    // 超上限是失败
    EXPECT_EQ(rtdbd_session_subs_add(&s, 33, 2u), RTDBD_SUBS_ADD_FAIL);
    EXPECT_EQ(rtdbd_session_subs_count(&s), 2u);
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));

    // 按序取点（快照分页顺序）
    uint32_t id = 0;
    ASSERT_TRUE(rtdbd_session_subs_get(&s, 0, &id));
    EXPECT_EQ(id, 11u);
    ASSERT_TRUE(rtdbd_session_subs_get(&s, 1, &id));
    EXPECT_EQ(id, 22u);
    EXPECT_FALSE(rtdbd_session_subs_get(&s, 2, &id));
    rtdbd_session_free(&s);
}

TEST(Session, SubsRemoveKeepsBothViewsConsistent) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    ASSERT_EQ(rtdbd_session_subs_add(&s, 11, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    ASSERT_EQ(rtdbd_session_subs_add(&s, 22, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    ASSERT_EQ(rtdbd_session_subs_add(&s, 33, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    rtdbd_session_subs_remove(&s, 22);
    EXPECT_EQ(rtdbd_session_subs_count(&s), 2u);
    EXPECT_FALSE(rtdbd_session_subs_contains(&s, 22));
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 11));
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 33));
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));
    // 退订后重新订阅：首值必发（RBE 状态已清）
    EXPECT_TRUE(rtdbd_subtab_decide(&s.sub, 22, 1.0, nullptr));
    rtdbd_session_free(&s);
}

TEST(Session, InstallSubsIsAtomic) {
    rtdbd_session_t s;
    ASSERT_TRUE(rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP));
    ASSERT_EQ(rtdbd_session_subs_add(&s, 11, RTDBD_SUB_MAX), RTDBD_SUBS_ADD_OK);
    const uint32_t ids[] = {1, 2, 3};
    EXPECT_TRUE(rtdbd_session_install_subs(&s, ids, 3));
    EXPECT_EQ(rtdbd_session_subs_count(&s), 3u);
    EXPECT_FALSE(rtdbd_session_subs_contains(&s, 11));
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 1));
    EXPECT_TRUE(rtdbd_session_subs_contains(&s, 3));
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));
    // 空集替换
    EXPECT_TRUE(rtdbd_session_install_subs(&s, nullptr, 0));
    EXPECT_EQ(rtdbd_session_subs_count(&s), 0u);
    EXPECT_TRUE(rtdbd_session_subs_consistent(&s));
    rtdbd_session_free(&s);
}

TEST(DeferQueue, PreservesFifoOrder) {
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    EXPECT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_RECONNECT, 1, 0));
    EXPECT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_REBUILD_SUBS, 1, 3));
    EXPECT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_TRIGGER_FULLSYNC, 2, 9));
    EXPECT_EQ(rtdbd_defer_count(&q), 3u);
    rtdbd_defer_action_t a{};
    ASSERT_TRUE(rtdbd_defer_pop(&q, &a));
    EXPECT_EQ(a.kind, RTDBD_DEFER_RECONNECT);
    EXPECT_EQ(a.session_id, 1u);
    ASSERT_TRUE(rtdbd_defer_pop(&q, &a));
    EXPECT_EQ(a.kind, RTDBD_DEFER_REBUILD_SUBS);
    EXPECT_EQ(a.arg, 3u);
    ASSERT_TRUE(rtdbd_defer_pop(&q, &a));
    EXPECT_EQ(a.kind, RTDBD_DEFER_TRIGGER_FULLSYNC);
    EXPECT_EQ(a.session_id, 2u);
    EXPECT_EQ(a.arg, 9u);
    EXPECT_EQ(rtdbd_defer_count(&q), 0u);
}

TEST(DeferQueue, RejectsWhenFullWithoutSilentDrop) {
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    for (uint32_t i = 0; i < RTDBD_DEFER_QUEUE_CAP; ++i) {
        ASSERT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_CLOSE, i, 0));
    }
    EXPECT_FALSE(rtdbd_defer_push(&q, RTDBD_DEFER_CLOSE, 999, 0));
    EXPECT_EQ(q.dropped, 1u);
    EXPECT_EQ(rtdbd_defer_count(&q), RTDBD_DEFER_QUEUE_CAP);
    // 已入队的动作不得因后来的拒绝而丢失：满队列仍按 FIFO 出队
    rtdbd_defer_action_t a{};
    EXPECT_TRUE(rtdbd_defer_pop(&q, &a));
    EXPECT_EQ(a.session_id, 0u);
}

TEST(DeferQueue, IsDeferredNotImmediate) {
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    EXPECT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_CLOSE, 3, 0));
    EXPECT_EQ(q.executed, 0u); // 入队仅登记，未执行
    EXPECT_EQ(q.enqueued, 1u);
    rtdbd_defer_action_t a{};
    ASSERT_TRUE(rtdbd_defer_pop(&q, &a));
    EXPECT_EQ(q.executed, 1u); // 出队才交付主循环循环尾
    EXPECT_EQ(a.kind, RTDBD_DEFER_CLOSE);
    EXPECT_EQ(a.session_id, 3u);
}
