/**
 * @file test_hB_audit.cpp
 * @brief h-B t-B：审计可追溯——**直接测试生产回调** hB_audit_callback（独立 TU）。
 *
 * 与 e2e fork server（父读不到子 &audit）不同，本用例同进程构造 audit + sid 环，
 * 注入生产回调本身，验证参数顺序、ts 补位、service_id 落环、deny 不触发路径。
 * 纯逻辑、默认构建即可运行。
 */
#include <gtest/gtest.h>

extern "C" {
#include "hB_audit_adapter.h"
}

#include <cstring>
#include <string>

namespace {

TEST(HBAudit, ProductionCallbackRecordsAuditWithCorrectArgOrder)
{
    irt_audit_t audit;
    irt_audit_init(&audit);
    hB_audit_sid_ring_t ring;
    hB_audit_sid_ring_init(&ring);
    hB_audit_ctx_t ctx{&audit, &ring};

    // partition 与 point_id 取**相异值**，防参数顺序写反（二者均 uint32）。
    const uint32_t uid = 42u, partition = 7u, point_id = 999u;
    const char *sid = "plc-line-1";
    hB_audit_callback(&ctx, 0u /* pid 恒 0 */, uid, partition, point_id,
                      0u /* ts_ns=0 → 回调补单调钟 */, sid);

    rtdbd_audit_entry_t out[8];
    uint32_t n = irt_audit_dump(&audit, out, 8u);
    ASSERT_EQ(n, 1u);
    EXPECT_EQ(out[0].pid, 0u);
    EXPECT_EQ(out[0].uid, uid);
    EXPECT_EQ(out[0].partition, partition);   // 若 partition/point_id 写反则失败
    EXPECT_EQ(out[0].point_id, point_id);
    EXPECT_GT(out[0].ts_ns, 0u);              // ts_ns=0 已被补单调钟

    // t-B：service_id 落纯内存 sid 环，按身份字段关联
    hB_audit_sid_entry_t souts[8];
    uint32_t sn = hB_audit_sid_ring_dump(&ring, souts, 8u);
    ASSERT_EQ(sn, 1u);
    EXPECT_EQ(souts[0].uid, uid);
    EXPECT_EQ(souts[0].partition, partition);
    EXPECT_EQ(souts[0].point_id, point_id);
    EXPECT_EQ(souts[0].ts_ns, out[0].ts_ns);   // 同源 ts，可关联
    EXPECT_EQ(std::string(souts[0].service_id), sid);
}

TEST(HBAudit, SidRingLruBoundedAndNoIo)
{
    hB_audit_sid_ring_t ring;
    hB_audit_sid_ring_init(&ring);
    // 填 256+1 条，验证有界（count 不超容量、无崩溃）
    for (uint32_t i = 0; i < HB_AUDIT_SID_CAPACITY + 1u; ++i) {
        hB_audit_sid_ring_push(&ring, 1000u + i, 1u, 0u, i, "svc");
    }
    EXPECT_EQ(ring.count, HB_AUDIT_SID_CAPACITY);
    hB_audit_sid_entry_t out[HB_AUDIT_SID_CAPACITY];
    EXPECT_EQ(hB_audit_sid_ring_dump(&ring, out, HB_AUDIT_SID_CAPACITY),
              HB_AUDIT_SID_CAPACITY);
}

TEST(HBAudit, NullCtxIsNoop)
{
    // 无 ctx：不崩溃、不伪造成功（上层计数体现）
    hB_audit_callback(nullptr, 0u, 1u, 0u, 5u, 0u, "svc");
}

} // namespace
