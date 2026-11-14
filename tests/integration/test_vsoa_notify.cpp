/**
 * @file test_vsoa_notify.cpp
 * @brief v3.12 M3.3 切片5：IR5_NotifyDirection 端到端 NOTIFY 定向（ON-only）。
 *
 * 依据 docs/03-设计文档/17-2026-11-12-IR5端到端通知定向方案.md（v3，评审通过）。
 *
 * ⚠️ IR5 = P9 部分闭环（用户裁决 q1）：本用例只验证 NOTIFY 定向（doc 13 §8.3-5
 * 裁决的一半）；SNAPSHOT_DONE 生产定向按裁决移出本切片。P9 四字段在
 * v3.12-B段数字落库.json 全部保持 pending，本用例测的是「测试内计数快照」，
 * 不是 B 段数字落库；注册 ctest 名 ≠ §11.1 P9 判据闭环、≠ §11.2 IR 注册判据闭环
 * （仍缺 6 个 required_irs 名字）。本文件严格保持「部分闭环」口径、不作任何
 * 超前宣称。
 *
 * 取证通道（q2）：fork 前 mmap(MAP_SHARED|MAP_ANONYMOUS) 共享页；只用 GCC
 * __atomic_* builtins（禁 _Atomic / stdatomic.h —— CODEBUDDY.md 红线：SylixOS
 * 支持不完整）。seq 奇偶 = seqlock 写中/稳定；父进程 probe_stable_read 按
 * irt_seqlock.h:52-60 范式（s0=load(ACQUIRE)→奇数重试→拷贝→fence(ACQUIRE)→
 * s1=load(ACQUIRE)→s0!=s1 重试）。snap_seq 仅诊断、不承担屏障。
 *
 * 可证伪判据（§3.2）：
 *  ① A 收到 1 帧定向 NOTIFY（point_id=5、值正确、quick==false）；
 *  ② B/C 同步点后各 N>=300 轮 select(10ms)+input_fds 负向观察窗，窗内持续
 *     ASSERT_FALSE(received)；同步点前连接/URL 订阅/hello/pump 逐项 ASSERT 正向保证；
 *  ③ init_snap 全 0 + hits 精确增量（expected=init.hits+1，非硬编码）；
 *  ④ 死区用例 misses 增量==1 且 A 不再收；
 *  ⑤ peer_notify_leak==0（辅助观测、不独立证伪）+ child_fail==0 + wstatus 正常
 *     （WIFSIGNALED && SIGTERM；WIFEXITED 任意码含 0 判 FAIL；kill 前 WNOHANG
 *      提前退出判 FAIL）。
 *
 * 测试拓扑（§3.3）：A 订阅者（订 URL + 点位 5，plc，应收）；B 非订阅者（订 URL
 * 但不订点位，plc，不收）；C sync_peer（订 URL + 点位，gate ② 层挡，不收）；
 * admin（SYNC_ADMIN）经 /sync/peers/set 注入 127.0.0.1 供 C 的来源校验。
 * 代表性局限（q3，如实披露）：C 依赖 allow_unauthenticated 非生产开关在 UNKNOWN
 * 态先订阅、再 hello 成 sync_peer，不等价于生产完整鉴权链，列为后续待办。
 *
 * 用例隔离（§3.1 M-2）：notify_counters 是 adapter 全局累计（calloc 全 0），
 * 故每个 TEST 独立 fork + 独立 mmap 页 + 独立 handle 实例 + 三类会话拓扑重建，
 * init_snap 取自各自 server，避免共用 server 使 hits==1 恒红。
 *
 * 留白（本机测不到、不声称 PASS）：生产完整鉴权链、SNAPSHOT_DONE 生产定向、
 * B 段数字、设备级 IR、SDK 侧 libvsoa 1.9.1 源码未经实读的推断（由 (a) 注入
 * 在同一 B/C 观察窗实测补证）、subtab 状态逐字段观测（门控接线自动化回归
 * 只部分覆盖）。
 *
 * TDD（§4 步骤 3/4）：步骤 3 RED 骨架把「循环内刷新 last_snap」留空 → 主判据
 * hits 轮询 2s 超时真实 FAIL；步骤 4 GREEN 补齐取证通道刷新（getter 在临界区
 * 外取局部 tmp，临界区内只做纯内存拷贝，严格贴合「临界区禁外部回调」）。
 * 判据（断言）在 RED/GREEN 完全相同，只差取证通道实现，不构成弱化判据。
 */
#include <gtest/gtest.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <netinet/in.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>

extern "C" {
#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>
#include "dispatch/rtdbd_dispatch.h"
#include "vsoa/vsoa_adapter.h"
#include "session/rtdbd_session.h"
}

namespace {

constexpr uint32_t kProbeMagic    = 0x4E4F5449u; /* "NOTI"：子进程初始化完成 */
constexpr int      kServerLoopMs  = 2000;        /* 子进程单轮 select 超时 */
constexpr int      kReplyWaitMs   = 5000;        /* 父进程等 RPC reply 上限 */
constexpr int      kProbeTimeoutMs = 2000;       /* magic / 计数原子轮询上限（2s） */
constexpr int      kNegWindowIters = 320;        /* B/C 负向观察窗 N >= 300 */
constexpr uint32_t kE2eUid        = 1000;        /* 本机联调默认 uid */
constexpr uint8_t  kStatusOk      = 0x80;        /* 0x80 + RTDBD_ST_OK */

/* 方案 §3.1：锁死 32B（4 x uint64_t 无 padding），防「初始全 0」判据失真。
 * C++17 用标准 static_assert，等价于方案文字的 _Static_assert(sizeof==32)。 */
static_assert(sizeof(rtdbd_notify_counters_t) == 32,
              "rtdbd_notify_counters_t must be 32B (4 x uint64_t)");

/* ---- 跨进程取证共享页（fork 前 mmap，MAP_ANONYMOUS 内核清零）---- */
struct notify_probe_t {
    uint32_t magic;       /* 原子：0x4E4F5449 = 子进程已完成初始化 */
    uint32_t child_fail;  /* 原子：子进程内 CHILD_ASSERT 失败计数 */
    uint32_t seq;         /* 原子 seqlock 序号：奇数写中 / 偶数稳定 */
    uint32_t snap_seq;    /* 原子诊断量：绝不承担屏障 */
    rtdbd_notify_counters_t init_snap;  /* create adapter 后、任何写之前 */
    rtdbd_notify_counters_t last_snap;  /* 每轮「有输入事件」后刷新 */
};

/* 子进程专用：CHILD_ASSERT 失败 → child_fail++(RELEASE) + _exit(2)。
 * 使父进程可归因（不继续跑 [[noreturn]] 循环），且 mmap 页初始全 0 不会
 * 让「child_fail==0」恒绿（wstatus / WNOHANG 是独立兜底判据）。 */
notify_probe_t *g_probe = nullptr;
#define CHILD_ASSERT(cond)                                                    \
    do {                                                                      \
        if (!(cond)) {                                                        \
            __atomic_fetch_add(&g_probe->child_fail, 1, __ATOMIC_RELEASE);    \
            _exit(2);                                                         \
        }                                                                     \
    } while (0)

/* 写端 seqlock：奇数(RELEASE) → 纯内存拷贝 → snap_seq++(RELEASE) → 偶数(RELEASE)。
 * 临界区内**不调用任何外部回调**（getter 结果须由调用方在区外取局部 tmp 传入）。 */
void probe_publish(notify_probe_t *p, const rtdbd_notify_counters_t *snap,
                   bool write_init)
{
    uint32_t s0 = __atomic_load_n(&p->seq, __ATOMIC_RELAXED);
    __atomic_store_n(&p->seq, s0 + 1u, __ATOMIC_RELEASE); /* 奇数：写中 */
    if (write_init) {
        p->init_snap = *snap;
    }
    p->last_snap = *snap;                                 /* 32B 普通拷贝 */
    __atomic_fetch_add(&p->snap_seq, 1u, __ATOMIC_RELEASE); /* 诊断 */
    __atomic_store_n(&p->seq, s0 + 2u, __ATOMIC_RELEASE); /* 偶数：发布 */
}

struct probe_snapshot_t {
    rtdbd_notify_counters_t init{};
    rtdbd_notify_counters_t last{};
    uint32_t snap_seq = 0;
    uint32_t child_fail = 0;
};

/* 父进程稳定读：命名 probe_stable_read（注意②：避免与已移除的
 * irt_seqlock_read() 混淆），严格对齐 irt_seqlock.h:52-60 范式。 */
probe_snapshot_t probe_stable_read(notify_probe_t *p)
{
    probe_snapshot_t out;
    for (;;) {
        uint32_t s0 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if (s0 & 1u) {
            continue; /* 写中，重试 */
        }
        out.init = p->init_snap;
        out.last = p->last_snap;
        out.snap_seq = __atomic_load_n(&p->snap_seq, __ATOMIC_ACQUIRE);
        out.child_fail = __atomic_load_n(&p->child_fail, __ATOMIC_ACQUIRE);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t s1 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
        if (s0 == s1) {
            break;
        }
    }
    return out;
}

/* 主屏障 1：等 magic（1ms 轮询 / 2s 超时 / 超时 FAIL）。 */
bool probe_wait_magic(notify_probe_t *p)
{
    for (int i = 0; i < kProbeTimeoutMs; ++i) {
        if (__atomic_load_n(&p->magic, __ATOMIC_ACQUIRE) == kProbeMagic) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

enum class counter_kind_t { hits, misses };

/* 主屏障 2：原子轮询稳定读 last_snap 计数 >= expected（1ms / 2s / 超时 FAIL）。
 * snap_seq 只是诊断量，不参与条件判断。 */
bool probe_wait_counter(notify_probe_t *p, counter_kind_t kind,
                        uint64_t expected, probe_snapshot_t *out)
{
    for (int i = 0; i < kProbeTimeoutMs; ++i) {
        *out = probe_stable_read(p);
        uint64_t v = (kind == counter_kind_t::hits) ? out->last.rbe_gate_hits
                                                    : out->last.rbe_gate_misses;
        if (v >= expected) {
            return true;
        }
        usleep(1000);
    }
    return false;
}

/* ---- 服务端能力配置 / 策略 / 审计（照 test_vsoa_e2e.cpp 联调范式）---- */
irt_policy_t *ir5_policy()
{
    static irt_policy_t *p = nullptr;
    if (!p) {
        p = new irt_policy_t();
        irt_policy_init(p);
        const char *path = "/tmp/indurtdb_ir5_policy.txt";
        FILE *f = std::fopen(path, "w");
        if (f) {
            std::fputs("1000:*:0:999\n", f);
            std::fclose(f);
        }
        uint32_t line = 0;
        irt_policy_load(p, path, &line);
    }
    return p;
}

rtdbd_conf_t *ir5_conf()
{
    static rtdbd_conf_t *c = nullptr;
    if (!c) {
        c = new rtdbd_conf_t();
        rtdbd_conf_init(c);
    }
    return c;
}

int g_audit_count = 0;
void ir5_audit(void *ctx, uint32_t pid, uint32_t uid, uint32_t partition,
               uint32_t point_id, uint64_t ts_ns, const char *service_id)
{
    (void)ctx; (void)pid; (void)uid; (void)partition;
    (void)point_id; (void)ts_ns; (void)service_id;
    g_audit_count++;
}

rtdbd_dispatch_cfg_t ir5_server_cfg()
{
    rtdbd_dispatch_cfg_t cfg;
    cfg.max_clients = 128;
    cfg.initial_clients = 32;
    cfg.max_subs_per_conn = 10000;
    cfg.initial_subs_per_conn = 2048;
    cfg.max_batch = 1024;
    return cfg;
}

int ir5_handler(void *ctx, uint16_t opcode, const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    (void)ctx;
    return static_cast<int>(rtdbd_dispatch(opcode, req, req_len, resp, resp_cap, resp_len));
}

/* 子进程：起 VSOA server（adapter），写 init_snap → 发布 magic → 事件循环。
 * token 表在**子进程 run_server 内**登记（与 test_vsoa_e2e.cpp:130-133 同处）。
 * 步骤 3 RED：循环内「刷新 last_snap」尚未接线（步骤 4 GREEN 补齐）。 */
[[noreturn]] void run_server(int ready_fd, indurtdb_t *handle, notify_probe_t *probe)
{
    g_probe = probe;
    const char *fi_env = std::getenv("IR5_FI");
    const char *fi = fi_env ? fi_env : "";

    rtdbd_vsoa_handler_t handler;
    handler.handle = ir5_handler;
    handler.ctx = nullptr;
    rtdbd_vsoa_adapter_t *adapter = rtdbd_vsoa_adapter_create(&handler);
    CHILD_ASSERT(adapter != nullptr);

    rtdbd_dispatch_cfg_t cfg = ir5_server_cfg();
    rtdbd_vsoa_adapter_set_cfg(adapter, &cfg);
    rtdbd_vsoa_adapter_set_handle(adapter, handle);
    rtdbd_vsoa_adapter_set_policy(adapter, ir5_policy());
    rtdbd_vsoa_adapter_set_conf(adapter, ir5_conf());
    rtdbd_vsoa_adapter_set_audit(adapter, ir5_audit, nullptr);
    rtdbd_vsoa_adapter_set_default_uid(adapter, kE2eUid);

    /* plc（A/B 用）/ sync_admin（注入成员表）/ sync_peer（C 用） */
    CHILD_ASSERT(rtdbd_vsoa_adapter_add_token(adapter, "tok-plc-1", kE2eUid, 0,
                     "plc-line-1", RTDBD_ROLE_PLC) == 0);
    CHILD_ASSERT(rtdbd_vsoa_adapter_add_token(adapter, "tok-admin-1", kE2eUid, 0,
                     "admin-console", RTDBD_ROLE_SYNC_ADMIN) == 0);
    CHILD_ASSERT(rtdbd_vsoa_adapter_add_token(adapter, "tok-peer-1", kE2eUid, 0,
                     "peer-node-1", RTDBD_ROLE_SYNC_PEER) == 0);
    /* 非生产开关：仅本机联调（q3，代表性局限见文件头） */
    rtdbd_vsoa_adapter_set_allow_unauthenticated(adapter, true);

    /* getter 冒烟（区外取值，纯读无副作用）：create adapter 后计数全 0。
     * 写临界区内只做纯内存拷贝，严格贴合「Seqlock 临界区内禁止调用外部回调」。 */
    rtdbd_notify_counters_t init_tmp = rtdbd_vsoa_adapter_notify_counters(adapter);
    probe_publish(probe, &init_tmp, true);

    uint16_t port = 0;
    CHILD_ASSERT(rtdbd_vsoa_adapter_start(adapter, 0, &port));
    ssize_t w = write(ready_fd, &port, sizeof(port));
    (void)w;
    close(ready_fd);

    /* ① 故障注入：magic 未置（子进程未初始化 → 父进程超时 FAIL） */
    if (std::strcmp(fi, "magic_missing") != 0) {
        __atomic_store_n(&probe->magic, kProbeMagic, __ATOMIC_RELEASE);
    }
    /* ④/⑤ 崩溃类与 child_fail 置位类注入的触发点一律放在循环内
     * 「本次 write 的 hits 已刷新」之后（见下），使父进程先完成主屏障与 B/C
     * 观察窗，再由 WNOHANG / WIFEXITED / child_fail 判据精确兜底。 */
    bool fi_fired = false;

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)rtdbd_vsoa_adapter_fds(adapter, &rfds);
        struct timeval tv;
        tv.tv_sec = kServerLoopMs / 1000;
        tv.tv_usec = (kServerLoopMs % 1000) * 1000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            rtdbd_vsoa_adapter_input(adapter, &rfds);

            /* 步骤 4 GREEN：取证通道循环刷新。
             * 实现期注意①：getter 在 seqlock 写临界区**外**取局部 tmp（纯读无
             * 副作用），临界区内只做纯内存拷贝 —— 严格贴合「Seqlock 临界区内
             * 禁止调用外部回调」。 */
            rtdbd_notify_counters_t tmp = rtdbd_vsoa_adapter_notify_counters(adapter);

            /* ③ 故障注入：getter 未调用（tmp 恒全 0 → hits 增量 0 → 超时 FAIL）。
             * 与 ② 的区别：本注入仍推进 snap_seq，但计数恒 0。 */
            if (std::strcmp(fi, "no_getter") == 0) {
                std::memset(&tmp, 0, sizeof(tmp));
            }
            /* ② 故障注入：终态快照不刷新（跳过 probe_publish → 超时 FAIL）。 */
            if (std::strcmp(fi, "no_last_refresh") != 0) {
                /* 写端：seq 奇数(RELEASE) → 写 last_snap(纯内存拷贝) →
                 * snap_seq++(RELEASE) → seq 偶数(RELEASE) 发布。 */
                probe_publish(probe, &tmp, false);
            }
            /* ④ / ⑤(i) 崩溃类注入：触发点放在「本次 write 的 hits 已刷新」之后，
             * 让父进程先完成主屏障（write reply + A 收 datagram + hits>=expected
             * 轮询）与 B/C 负向观察窗，最终由 finish_and_check_wstatus 的
             * WNOHANG 提前退出 + WIFEXITED 判据捕获 → 精确验证这两个兜底判据，
             * 而非仅仅 connect 阶段失败。两种注入下 child_fail 均保持 0
             * （mmap 页初始全 0），正是方案 ⑤ 要求证伪的「child_fail==0 恒绿」
             * 场景：单靠 child_fail 会漏判，必须靠 wstatus / WNOHANG 兜底。
             * 延迟一小段确保 reply/datagram 已由内核冲刷给父进程。 */
            if (tmp.rbe_gate_hits >= 1 && !fi_fired) {
                fi_fired = true;
                if (std::strcmp(fi, "early_crash") == 0) {
                    usleep(120000);
                    _exit(7); /* ④ server 提前崩溃（异常退出码） */
                }
                if (std::strcmp(fi, "child_fail_unset") == 0) {
                    usleep(120000);
                    _exit(2); /* ⑤(i) 模拟断言失败但 child_fail 未置位，仍 _exit(2) */
                }
                if (std::strcmp(fi, "child_fail_no_exit") == 0) {
                    /* ⑤(ii) 断言失败但 child_fail 已置位却未 _exit：子进程继续
                     * 正常服务，父进程 final_snap.child_fail!=0 断言应 FAIL。 */
                    __atomic_fetch_add(&probe->child_fail, 1, __ATOMIC_RELEASE);
                }
            }
            /* 超时轮（r<=0）不刷新、不推进 snap_seq —— 消除 v2 的
             * 「无关轮次提前满足条件」假红；snap_seq 只作诊断、不承担屏障。 */
        }
    }
}

/* ---- VSOA client 回调与结果 ---- */
struct datagram_result_t {
    bool received = false;
    int frames = 0;         /* 收到的定向（非 quick）NOTIFY 帧数 */
    int quick_frames = 0;   /* 非定向通道（quick）帧数：须为 0 */
    uint32_t point_id = 0;
    uint8_t type = 0;
    int32_t value = 0;
};

void on_datagram(void *arg, vsoa_client_t *client, vsoa_url_t *url,
                 vsoa_payload_t *payload, bool quick)
{
    (void)client;
    (void)url;
    datagram_result_t *d = static_cast<datagram_result_t *>(arg);
    if (quick) {
        d->quick_frames++;   /* 非定向通道：记录但绝不置 received */
        return;
    }
    if (!payload || !payload->data || payload->data_len < sizeof(rtdbd_notify_t)) {
        return;
    }
    rtdbd_notify_t nt;
    std::memset(&nt, 0, sizeof(nt));
    std::memcpy(&nt, payload->data, sizeof(nt));
    if (d->frames == 0) {
        d->point_id = nt.point_id;
        d->type = nt.type;
        std::memcpy(&d->value, &nt.value_bits, sizeof(d->value));
    }
    d->frames++;
    d->received = true;
}

struct rpc_result_t {
    bool replied = false;
    uint8_t status = 0;
    size_t data_len = 0;
    uint8_t data[256]{};
};

void on_rpc(void *arg, vsoa_client_t *client, vsoa_header_t *hdr,
            vsoa_payload_t *payload)
{
    (void)client;
    rpc_result_t *res = static_cast<rpc_result_t *>(arg);
    res->replied = true;
    if (hdr) {
        res->status = hdr->status;
    }
    if (payload && payload->data && payload->data_len > 0) {
        res->data_len = payload->data_len < sizeof(res->data)
                            ? payload->data_len : sizeof(res->data);
        std::memcpy(res->data, payload->data, res->data_len);
    }
}

bool pump_reply(vsoa_client_t *c, rpc_result_t *res, int wait_ms)
{
    for (int e = 0; e < wait_ms && !res->replied; e += 10) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)vsoa_client_fds(c, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            vsoa_client_input_fds(c, &rfds);
        }
    }
    return res->replied;
}

/* 单个 client 的 datagram 泵（A 等收到 / M-5a B 等收到）。 */
void pump_datagram(vsoa_client_t *c, datagram_result_t *d, int rounds)
{
    for (int i = 0; i < rounds && !d->received; ++i) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)vsoa_client_fds(c, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            vsoa_client_input_fds(c, &rfds);
        }
    }
}

/* ② 确定性负向观察窗：B、C 各有独立 fd_set，各 N 轮 select(10ms)+input_fds，
 * 窗内持续 ASSERT_FALSE(received)。(a) 注入 RED 必须落在这个窗内。 */
void observe_bc_zero(vsoa_client_t *b, datagram_result_t *db,
                     vsoa_client_t *c, datagram_result_t *dc, int rounds)
{
    for (int i = 0; i < rounds; ++i) {
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            (void)vsoa_client_fds(b, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
            if (r > 0) {
                vsoa_client_input_fds(b, &rfds);
            }
        }
        {
            fd_set rfds;
            FD_ZERO(&rfds);
            (void)vsoa_client_fds(c, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
            if (r > 0) {
                vsoa_client_input_fds(c, &rfds);
            }
        }
        ASSERT_FALSE(db->received) << "B 收到 NOTIFY（观察窗轮 " << i << "）";
        ASSERT_FALSE(dc->received) << "C 收到 NOTIFY（观察窗轮 " << i << "）";
    }
}

/* ---- 会话辅助 ---- */
vsoa_client_t *make_client(uint16_t port, datagram_result_t *dg)
{
    vsoa_client_t *c = vsoa_client_create(nullptr, nullptr);
    if (!c) {
        return nullptr;
    }
    vsoa_client_on_datagram(c, on_datagram, dg);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    if (!vsoa_client_connect(c, reinterpret_cast<struct sockaddr *>(&addr),
                             sizeof(addr), &ts, nullptr, info, sizeof(info))) {
        vsoa_client_close(c);
        return nullptr;
    }
    return c;
}

bool cli_subscribe_url(vsoa_client_t *c, const char *u)
{
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(u);
    url.url_len = std::strlen(u);
    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    return vsoa_client_subscribe(c, &url, nullptr, nullptr, &ts);
}

bool cli_call(vsoa_client_t *c, const char *url_str, const void *req,
              size_t req_len, rpc_result_t *res)
{
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(url_str);
    url.url_len = std::strlen(url_str);
    vsoa_payload_t pl;
    std::memset(&pl, 0, sizeof(pl));
    if (req && req_len) {
        pl.data = const_cast<char *>(static_cast<const char *>(req));
        pl.data_len = req_len;
    }
    struct timespec cts;
    cts.tv_sec = 3;
    cts.tv_nsec = 0;
    *res = rpc_result_t{};
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                          on_rpc, res, &cts)) {
        return false;
    }
    return pump_reply(c, res, kReplyWaitMs);
}

bool cli_hello(vsoa_client_t *c, const char *token, const char *service_id,
               rpc_result_t *res)
{
    rtdbd_vsoa_hello_req_t h;
    std::memset(&h, 0, sizeof(h));
    std::snprintf(h.token, sizeof(h.token), "%s", token);
    std::snprintf(h.service_id, sizeof(h.service_id), "%s", service_id);
    return cli_call(c, "/indurtdb/v1/hello", &h, sizeof(h), res);
}

bool cli_sub_point(vsoa_client_t *c, uint32_t point_id, rpc_result_t *res)
{
    rtdbd_sub_req_t s;
    std::memset(&s, 0, sizeof(s));
    s.point_id = point_id;
    return cli_call(c, "/indurtdb/v1/subscription/add", &s, sizeof(s), res);
}

bool cli_write(vsoa_client_t *c, uint32_t point_id, int32_t v, rpc_result_t *res)
{
    rtdbd_write_req_t w;
    std::memset(&w, 0, sizeof(w));
    w.point_id = point_id;
    w.type = RTDBD_TYPE_INT32;
    std::memcpy(&w.value_bits, &v, sizeof(v));
    return cli_call(c, "/indurtdb/v1/point/write", &w, sizeof(w), res);
}

bool cli_admin_set_peer(vsoa_client_t *c, rpc_result_t *res)
{
    rtdbd_peers_resp_t set;
    std::memset(&set, 0, sizeof(set));
    set.count = 1;
    std::snprintf(set.peers[0].addr, sizeof(set.peers[0].addr), "127.0.0.1");
    std::snprintf(set.peers[0].node_id, sizeof(set.peers[0].node_id), "n0");
    set.peers[0].state = RTDBD_PEER_ST_ACTIVE;
    return cli_call(c, "/indurtdb/v1/sync/peers/set", &set, sizeof(set), res);
}

/* 三类会话 + admin 拓扑；A/B/C 各自独立 client + 独立 dg。
 * sub_a/sub_c 控制点位订阅（C 必须在 hello 之前、UNKNOWN 态订阅）。 */
struct topology_t {
    vsoa_client_t *a = nullptr;
    vsoa_client_t *b = nullptr;
    vsoa_client_t *c = nullptr;
    vsoa_client_t *admin = nullptr;
    datagram_result_t dga{};
    datagram_result_t dgb{};
    datagram_result_t dgc{};
    datagram_result_t dgadmin{};
    rpc_result_t rpc{};

    ~topology_t()
    {
        if (a)     { vsoa_client_close(a); }
        if (b)     { vsoa_client_close(b); }
        if (c)     { vsoa_client_close(c); }
        if (admin) { vsoa_client_close(admin); }
    }

    bool build(uint16_t port, bool sub_a, bool sub_c)
    {
        const char *kNotifyUrl = "/indurtdb/v1/notify";
        a = make_client(port, &dga);
        b = make_client(port, &dgb);
        c = make_client(port, &dgc);
        admin = make_client(port, &dgadmin);
        if (!a || !b || !c || !admin) {
            ADD_FAILURE() << "四类会话连接失败（正向保证）";
            return false;
        }
        /* /notify URL 订阅（SDK 层；B 订 URL 但不订点位，分离两层订阅） */
        if (!cli_subscribe_url(a, kNotifyUrl) || !cli_subscribe_url(b, kNotifyUrl) ||
            !cli_subscribe_url(c, kNotifyUrl)) {
            ADD_FAILURE() << "/notify URL 订阅失败（正向保证）";
            return false;
        }
        /* C 在 UNKNOWN 态先订点位（role→URL 门禁会拒 sync_peer 调
         * /subscription/add，故必须先订阅再 hello）。 */
        if (sub_c) {
            if (!cli_sub_point(c, 5, &rpc) || rpc.status != kStatusOk) {
                ADD_FAILURE() << "C(UNKNOWN) 订阅点位 5 失败（正向保证）";
                return false;
            }
        }
        /* admin hello + /sync/peers/set 注入 127.0.0.1（C 来源校验命中成员表） */
        if (!cli_hello(admin, "tok-admin-1", "admin-console", &rpc) ||
            rpc.status != kStatusOk) {
            ADD_FAILURE() << "admin(SYNC_ADMIN) hello 失败（正向保证）";
            return false;
        }
        if (!cli_admin_set_peer(admin, &rpc) || rpc.status != kStatusOk) {
            ADD_FAILURE() << "admin /sync/peers/set 注入 127.0.0.1 失败（正向保证）";
            return false;
        }
        /* A/B hello 成 plc（role=PLC，/subscription/add 允许） */
        if (!cli_hello(a, "tok-plc-1", "plc-line-1", &rpc) || rpc.status != kStatusOk ||
            !cli_hello(b, "tok-plc-1", "plc-line-1", &rpc) || rpc.status != kStatusOk) {
            ADD_FAILURE() << "A/B(plc) hello 失败（正向保证）";
            return false;
        }
        if (sub_a) {
            if (!cli_sub_point(a, 5, &rpc) || rpc.status != kStatusOk) {
                ADD_FAILURE() << "A 订阅点位 5 失败（正向保证）";
                return false;
            }
        }
        /* C 最后 hello 成 sync_peer（握手不清订阅，vsoa_adapter.c:186-192） */
        if (!cli_hello(c, "tok-peer-1", "peer-node-1", &rpc) || rpc.status != kStatusOk) {
            ADD_FAILURE() << "C(sync_peer) hello 失败（成员表来源校验，正向保证）";
            return false;
        }
        /* 拓扑就绪：给 server 一轮 input 机会处理所有订阅/hello（同步点前置条件）。 */
        usleep(120000);
        for (int i = 0; i < 20; ++i) {
            fd_set rfds;
            FD_ZERO(&rfds);
            (void)vsoa_client_fds(a, &rfds);
            (void)vsoa_client_fds(b, &rfds);
            (void)vsoa_client_fds(c, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
            if (r > 0) {
                vsoa_client_input_fds(a, &rfds);
                vsoa_client_input_fds(b, &rfds);
                vsoa_client_input_fds(c, &rfds);
            }
        }
        return true;
    }
};

/* 用例隔离：每 TEST 独立 handle 实例 + 独立 mmap 页 + 独立 fork。
 * RAII 保证 ASSERT_* fatal 时仍回收子进程 / munmap / 关 handle。 */
struct harness_t {
    indurtdb_t *handle = nullptr;
    notify_probe_t *probe = nullptr;
    pid_t pid = -1;
    bool reaped = false;
    std::string inst;

    ~harness_t()
    {
        if (pid > 0 && !reaped) {
            kill(pid, SIGTERM);
            int st = 0;
            waitpid(pid, &st, 0);
        }
        if (probe && probe != MAP_FAILED) {
            munmap(probe, sizeof(notify_probe_t));
        }
        if (handle) {
            indurtdb_h_close(handle);
        }
    }
};

/* 完成所有共享页读取后调用：kill 前 WNOHANG 提前退出 FAIL；
 * wstatus 正常 = WIFSIGNALED && SIGTERM；WIFEXITED 任意码（含 0）FAIL。 */
void finish_and_check_wstatus(harness_t *h, probe_snapshot_t *final_snap)
{
    int st_pre = 0;
    pid_t pre = waitpid(h->pid, &st_pre, WNOHANG);
    if (pre == 0) {
        ASSERT_EQ(kill(h->pid, SIGTERM), 0);
        int wstatus = 0;
        ASSERT_EQ(waitpid(h->pid, &wstatus, 0), h->pid);
        h->reaped = true;
        EXPECT_TRUE(WIFSIGNALED(wstatus)) << "wstatus 非 SIGTERM 终止";
        if (WIFSIGNALED(wstatus)) {
            EXPECT_EQ(WTERMSIG(wstatus), SIGTERM);
        }
        EXPECT_FALSE(WIFEXITED(wstatus))
            << "WIFEXITED 任意退出码（含 0）判 FAIL";
    } else {
        if (pre == h->pid) {
            h->reaped = true;
        }
        ADD_FAILURE() << "kill 前 WNOHANG 发现 server 提前退出（判 FAIL）";
        if (pre == h->pid && WIFEXITED(st_pre)) {
            ADD_FAILURE() << "WIFEXITED 任意退出码（含 0）判 FAIL，exit="
                          << WEXITSTATUS(st_pre);
        }
    }
    /* child_fail 判据：稳定读后断言 0（⑤ 注入下由 wstatus/WNOHANG 独立兜底）。 */
    EXPECT_EQ(final_snap->child_fail, 0u) << "子进程内 CHILD_ASSERT 有失败";
}

/* ===== 集成层：ON 侧 getter 冒烟（NULL → 全 0）+ 32B 布局锁死 ===== */
TEST(IR5NotifyDirection, GetterSmokeNullAndCountersSize)
{
    EXPECT_EQ(sizeof(rtdbd_notify_counters_t), 32u);
    rtdbd_notify_counters_t c = rtdbd_vsoa_adapter_notify_counters(nullptr);
    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);
    EXPECT_EQ(c.peer_notify_leak, 0u);
}

/* ===== 端到端层：主判据（①–③、⑤ + ② B/C 负向观察窗）===== */
TEST(IR5NotifyDirection, MainCriteriaDirectedNotifyIsolation)
{
    ::signal(SIGPIPE, SIG_IGN);
    harness_t h;
    h.inst = "ir5_main_" + std::to_string(static_cast<long long>(getpid()));
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    ASSERT_EQ(indurtdb_h_open(&h.handle, h.inst.c_str(), &cfg), 0);
    ASSERT_NE(h.handle, nullptr);
    ASSERT_EQ(indurtdb_h_create_point(h.handle, 5, "p5", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    void *m = mmap(nullptr, sizeof(notify_probe_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(m, MAP_FAILED) << "mmap 共享页失败";
    h.probe = static_cast<notify_probe_t *>(m);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], h.handle, h.probe);
    }
    h.pid = pid;
    close(pipefd[1]);
    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)),
              static_cast<ssize_t>(sizeof(port))) << "server 未回传 port";
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    /* 主屏障 1：等 magic（① 注入下 2s 超时 FAIL） */
    ASSERT_TRUE(probe_wait_magic(h.probe)) << "等 magic 超时（server 未初始化）";
    probe_snapshot_t init = probe_stable_read(h.probe);
    EXPECT_EQ(init.init.rbe_gate_hits, 0u);
    EXPECT_EQ(init.init.rbe_gate_misses, 0u);
    EXPECT_EQ(init.init.snapshot_done_caps_only, 0u);
    EXPECT_EQ(init.init.peer_notify_leak, 0u);
    EXPECT_EQ(init.child_fail, 0u);

    /* 三类会话拓扑（C UNKNOWN 态先订阅 + A 订阅点位） */
    topology_t topo;
    ASSERT_TRUE(topo.build(port, true, true)) << "拓扑构建失败";

    /* 同步点：write RPC reply（vsoa_adapter.c:496 dispatch 先于 :518 reply） */
    rpc_result_t wres;
    ASSERT_TRUE(cli_write(topo.a, 5, 777, &wres)) << "write RPC 无 reply";
    EXPECT_EQ(wres.status, kStatusOk);
    /* 等 A 收到定向 datagram */
    pump_datagram(topo.a, &topo.dga, kNegWindowIters);

    /* ① A 收到 1 帧、point_id=5、值正确、quick==false */
    ASSERT_TRUE(topo.dga.received) << "A 未收到定向 NOTIFY";
    EXPECT_EQ(topo.dga.frames, 1);
    EXPECT_EQ(topo.dga.point_id, 5u);
    EXPECT_EQ(topo.dga.type, static_cast<uint8_t>(RTDBD_TYPE_INT32));
    EXPECT_EQ(topo.dga.value, 777);
    EXPECT_EQ(topo.dga.quick_frames, 0) << "非定向（quick）通道出现帧";

    /* ③ 主屏障 2：原子轮询 hits >= expected（expected=init.hits+1，非硬编码） */
    const uint64_t expected = init.init.rbe_gate_hits + 1u;
    probe_snapshot_t snap;
    ASSERT_TRUE(probe_wait_counter(h.probe, counter_kind_t::hits, expected, &snap))
        << "轮询 last_snap.hits >= expected 超时（取证通道/门控异常）";
    EXPECT_EQ(snap.last.rbe_gate_hits, expected) << "hits 非精确增量（多发/少发）";

    /* ② B/C 确定性负向观察窗（同步点之后，N>=300，窗内持续 ASSERT_FALSE）。
     * (a) 注入 RED 必须在这个窗内变红。 */
    observe_bc_zero(topo.b, &topo.dgb, topo.c, &topo.dgc, kNegWindowIters);

    /* ⑤ 辅助观测 + 诊断：leak==0（辅助、不独立证伪）、snap_seq 仅打印 */
    probe_snapshot_t final_snap = probe_stable_read(h.probe);
    EXPECT_EQ(final_snap.last.peer_notify_leak, 0u)
        << "peer_notify_leak>0（(b) 注入预期变红项；辅助观测）";
    EXPECT_EQ(final_snap.child_fail, 0u);
    std::printf("[IR5/main] snap_seq=%u init_hits=%llu last_hits=%llu "
                "misses=%llu leak=%llu child_fail=%u\n",
                final_snap.snap_seq,
                static_cast<unsigned long long>(final_snap.init.rbe_gate_hits),
                static_cast<unsigned long long>(final_snap.last.rbe_gate_hits),
                static_cast<unsigned long long>(final_snap.last.rbe_gate_misses),
                static_cast<unsigned long long>(final_snap.last.peer_notify_leak),
                final_snap.child_fail);

    /* 所有共享页读取已在 kill 之前完成；kill 后只取 wstatus */
    finish_and_check_wstatus(&h, &final_snap);
}

/* ===== ④ 死区用例：首值必发(hits+1)，同值二次写 misses 增量==1 且 A 不再收 ===== */
TEST(IR5NotifyDirection, DeadbandSuppressesSameValueSecondWrite)
{
    ::signal(SIGPIPE, SIG_IGN);
    harness_t h;
    h.inst = "ir5_dead_" + std::to_string(static_cast<long long>(getpid()));
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    ASSERT_EQ(indurtdb_h_open(&h.handle, h.inst.c_str(), &cfg), 0);
    ASSERT_NE(h.handle, nullptr);
    ASSERT_EQ(indurtdb_h_create_point(h.handle, 5, "p5", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    /* fork 前设死区：父子共享同一 shm，rtdbd_dispatch.c:86 从 shm 读 meta */
    indurtdb_meta_t meta;
    std::memset(&meta, 0, sizeof(meta));
    meta.deadband = 1.0f;
    meta.flags = INDURTDB_META_FLAG_DEADBAND;
    ASSERT_EQ(indurtdb_h_set_meta(h.handle, 5, &meta), 0);

    void *m = mmap(nullptr, sizeof(notify_probe_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(m, MAP_FAILED);
    h.probe = static_cast<notify_probe_t *>(m);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], h.handle, h.probe);
    }
    h.pid = pid;
    close(pipefd[1]);
    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)),
              static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    ASSERT_TRUE(probe_wait_magic(h.probe));
    probe_snapshot_t init = probe_stable_read(h.probe);
    EXPECT_EQ(init.init.rbe_gate_hits, 0u);
    EXPECT_EQ(init.init.rbe_gate_misses, 0u);

    topology_t topo;
    ASSERT_TRUE(topo.build(port, true, true));

    /* 首写：首值必发 → hits+1、A 收 1 帧 */
    rpc_result_t w1;
    ASSERT_TRUE(cli_write(topo.a, 5, 100, &w1));
    EXPECT_EQ(w1.status, kStatusOk);
    pump_datagram(topo.a, &topo.dga, kNegWindowIters);
    ASSERT_TRUE(topo.dga.received) << "首值必发：A 应收 1 帧";
    EXPECT_EQ(topo.dga.frames, 1);
    const uint64_t expected_hits = init.init.rbe_gate_hits + 1u;
    probe_snapshot_t s1;
    ASSERT_TRUE(probe_wait_counter(h.probe, counter_kind_t::hits, expected_hits, &s1));
    EXPECT_EQ(s1.last.rbe_gate_hits, expected_hits);

    /* 同值二次写：DEADBAND 抑制 → misses 增量==1，A 不再收 */
    rpc_result_t w2;
    ASSERT_TRUE(cli_write(topo.a, 5, 100, &w2));
    EXPECT_EQ(w2.status, kStatusOk);
    const uint64_t expected_misses = init.init.rbe_gate_misses + 1u;
    probe_snapshot_t s2;
    ASSERT_TRUE(probe_wait_counter(h.probe, counter_kind_t::misses,
                                   expected_misses, &s2))
        << "死区 misses 增量未达预期";
    EXPECT_EQ(s2.last.rbe_gate_misses, expected_misses);
    EXPECT_EQ(s2.last.rbe_gate_hits, expected_hits) << "同值二次写不应再 hits++";
    /* A 不再收：二次写后再泵一轮，frames 仍为 1 */
    datagram_result_t after;
    (void)after;
    for (int i = 0; i < 120; ++i) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)vsoa_client_fds(topo.a, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            vsoa_client_input_fds(topo.a, &rfds);
        }
    }
    EXPECT_EQ(topo.dga.frames, 1) << "死区同值二次写 A 不应再收";

    probe_snapshot_t final_snap = probe_stable_read(h.probe);
    EXPECT_EQ(final_snap.last.peer_notify_leak, 0u);
    finish_and_check_wstatus(&h, &final_snap);
}

/* ===== 集成层：订阅门禁行为回归（非顺序不变量独立守卫，见第二签 I-1）===== */
TEST(IR5NotifyDirection, UnsubscribedSessionKeepsFreshRbeState)
{
    ::signal(SIGPIPE, SIG_IGN);
    harness_t h;
    h.inst = "ir5_m5a_" + std::to_string(static_cast<long long>(getpid()));
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    ASSERT_EQ(indurtdb_h_open(&h.handle, h.inst.c_str(), &cfg), 0);
    ASSERT_NE(h.handle, nullptr);
    ASSERT_EQ(indurtdb_h_create_point(h.handle, 5, "p5", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    void *m = mmap(nullptr, sizeof(notify_probe_t), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    ASSERT_NE(m, MAP_FAILED);
    h.probe = static_cast<notify_probe_t *>(m);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], h.handle, h.probe);
    }
    h.pid = pid;
    close(pipefd[1]);
    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)),
              static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    ASSERT_TRUE(probe_wait_magic(h.probe));
    probe_snapshot_t init = probe_stable_read(h.probe);

    /* A 订阅点位（首写必发参照），B 初始不订阅点位、C 不订阅点位。 */
    topology_t topo;
    ASSERT_TRUE(topo.build(port, true, false));

    /* 第一次写：B 未订阅态经历一次写 → B 不收（仅验订阅门禁，不验 RBE 状态） */
    rpc_result_t w1;
    ASSERT_TRUE(cli_write(topo.a, 5, 100, &w1));
    EXPECT_EQ(w1.status, kStatusOk);
    for (int i = 0; i < 150; ++i) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)vsoa_client_fds(topo.b, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            vsoa_client_input_fds(topo.b, &rfds);
        }
    }
    EXPECT_FALSE(topo.dgb.received) << "B 未订阅点位时不应收 NOTIFY";

    /* B 再订阅点位 5 */
    rpc_result_t sres;
    ASSERT_TRUE(cli_sub_point(topo.b, 5, &sres));
    EXPECT_EQ(sres.status, kStatusOk);
    usleep(80000);

    /* 下次首写：B 必须收到（首值必发）；非「未消耗 RBE 状态」的独立证伪 */
    rpc_result_t w2;
    ASSERT_TRUE(cli_write(topo.a, 5, 200, &w2));
    EXPECT_EQ(w2.status, kStatusOk);
    pump_datagram(topo.b, &topo.dgb, kNegWindowIters);
    ASSERT_TRUE(topo.dgb.received) << "M-5a：B 订阅后首写必须收到";
    EXPECT_EQ(topo.dgb.point_id, 5u);
    EXPECT_EQ(topo.dgb.value, 200);
    EXPECT_EQ(topo.dgb.quick_frames, 0);
    (void)init;

    probe_snapshot_t final_snap = probe_stable_read(h.probe);
    finish_and_check_wstatus(&h, &final_snap);
}

} // namespace
