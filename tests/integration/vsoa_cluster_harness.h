/**
 * @file vsoa_cluster_harness.h
 * @brief v3.12 M2：6 节点编排 + 故障注入 harness（IR1–IR7 的**基础设施**）。
 *
 * 定位与边界（如实说明，勿夸大）：
 * - 本 harness 提供**节点编排**与**故障注入**能力：起停多节点 VSOA 服务、端口自动
 *   分配、按节点施加故障（崩溃 / 暂停 / 恢复）、按节点做健康探测（VSOA RPC ping）。
 * - 本 harness **不实现集群同步语义**：增量/全量同步、epoch 纪元隔离、分区收敛
 *   属 IR1–IR7 的实现期工作（依赖本 harness 作为载体）。用例须显式声明覆盖范围。
 * - 节点进程 = fork 出的子进程，各跑一个 VSOA adapter（与 prod 的 rtdbd 前端形态
 *   一致的前端形态，但**不含** rtdbd 主循环的 VSOA fd 合并 —— 该项仍为 M1 留白）。
 *
 * 纪律（doc 13 §10.2）：POC 临时验证代码不入库；本 harness 是**永久测试脚手架**，
 * 故入库，且作为 IR1–IR7 的复用底座。
 */
#ifndef VSOA_CLUSTER_HARNESS_H_
#define VSOA_CLUSTER_HARNESS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace vsoa_cluster {

/* 单节点状态。 */
struct Node {
    int         index = 0;
    pid_t       pid = -1;
    uint16_t    port = 0;
    std::string instance;
    std::string service_id;
    bool        alive = false;
};

/* 故障类型（§8.3-7 语义的最小可注入集合）。 */
enum class Fault {
    None,     /* 无故障 */
    Crash,    /* 进程崩溃（SIGKILL）：模拟节点掉电/进程死 */
    Pause,    /* 进程暂停（SIGSTOP）：模拟断链/网络分区（进程在但不通） */
};

class Harness {
public:
    Harness() = default;
    ~Harness();

    Harness(const Harness &) = delete;
    Harness &operator=(const Harness &) = delete;

    /* 起 n 个节点（每个节点独立共享内存实例 + 独立 VSOA 端口）。
     * 返回 false 表示任一节点启动失败（此时已起的节点会被 stop 清理）。 */
    bool start(int n);

    /* 停止全部节点（先 SIGCONT 再 SIGKILL，避免暂停态残留）。 */
    void stop();

    /* 施加故障：Crash=崩溃、Pause=暂停（断链）。 */
    bool inject(int index, Fault fault);

    /* 解除故障：Pause → SIGCONT 恢复；Crash 不可恢复（需 restart）。 */
    bool clear(int index);

    /* 重启节点（崩溃后恢复）：新 fork 一个子进程并重新分配端口。
     * ⚠ 端口会变化；订阅重建（§8.3-5）当前**未实现**（M1 留白），故重启后
     * 只能验证「节点恢复服务能力」，不能验证「订阅自动重建」。 */
    bool restart(int index);

    /* 节点健康探测：经 VSOA RPC 调 /ping，成功返回 true。
     * 故障节点（崩溃/暂停）应返回 false —— 这是故障注入是否生效的判据。 */
    bool probe(int index, int timeout_ms = 2000);

    size_t size() const { return nodes_.size(); }
    const Node &node(int index) const { return nodes_[index]; }
    const std::string &last_error() const { return last_error_; }

private:
    bool spawn_node(Node &n);
    std::vector<Node> nodes_;
    std::string last_error_;
};

} // namespace vsoa_cluster

#endif /* VSOA_CLUSTER_HARNESS_H_ */
