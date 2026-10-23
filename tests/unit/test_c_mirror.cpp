/**
 * @file test_c_mirror.cpp
 * @brief T8 B1: 跨语言镜像一致性 —— Python 复刻布局/协议 vs C 头文件
 *
 * 目的：tools/irt_migrate 与 tools/rtdb-monitor 用纯 Python 逐字节复刻了
 * 共享内存布局与 rtdbd 二进制协议。C 侧一旦改动（加字段/改偏移/改尺寸），
 * Python 侧若不同步，迁移工具会静默写出错误段、监控台会解析错报文。
 * 本测试把 **C 真值**（offsetof / sizeof / 宏）与 **Python 真值**
 * （真实 import 的模块常量与结构体 .size）逐一比对。
 *
 * 关键点：Python 侧不重新抄写字面量，而是
 *   - 常量 / 结构体尺寸：直接 import 模块读取；
 *   - Header 字段偏移与 CRC 覆盖区间：**行为推导**——对 build_v2_header()
 *     产出的字节逐字节翻转，观察 parse_v2_header() 哪个字段变化，从而反推
 *     Python 实现实际使用的偏移，不依赖源码格式（避免格式化即误报）。
 *
 * Python 端缺失的常量（value/timestamp/type/... 偏移）不靠补常量覆盖，
 * 而是用派生不变式钉死（见 point record invariant chain）。
 */
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "gtest/gtest.h"
#include "rtdbd/protocol.h"

/* C 头文件未自带 extern "C"（库本身是纯 C），与其它单测一致地显式包裹，
 * 否则 offsetof/sizeof 可用但 irt_header_seal 等函数会因名字修饰链接失败。 */
extern "C" {
#include <indurtdb/indurtdb.h>

#include "core/irt_index.h"
#include "core/irt_shm.h"
#include "internal/irt_types.h"
}

#ifndef INDURTDB_TOOLS_DIR
#define INDURTDB_TOOLS_DIR ""
#endif

namespace {

std::string hexpair(unsigned long long v) {
    std::ostringstream o;
    o << v << " (0x" << std::hex << v << std::dec << ")";
    return o.str();
}

/* 累积所有不一致，最后一次性输出可读报告（而非一串裸 EXPECT_EQ）。 */
class MirrorCheck {
public:
    explicit MirrorCheck(const std::map<std::string, long long>& py) : py_(py) {}

    /* C 真值 vs Python 输出中的同名 KEY。 */
    void eq(const std::string& key, unsigned long long c) {
        auto it = py_.find(key);
        if (it == py_.end()) {
            failures_.push_back(key + ": MISSING from Python output (C="
                                + hexpair(c) + ")");
            return;
        }
        eq_raw(key, c, static_cast<unsigned long long>(it->second));
    }

    /* 两个已计算值比对：label 用于说明，c 来自 C 侧，p 来自 Python 侧。 */
    void eq_raw(const std::string& label, unsigned long long c,
                unsigned long long p) {
        if (c != p) {
            failures_.push_back(label + ": C=" + hexpair(c)
                                + " Python=" + hexpair(p));
        }
    }

    /* 取 Python 输出中的值；缺失时登记一次失败并返回 0（避免 .at 抛异常
     * 把可读报告变成崩溃）。 */
    unsigned long long v(const std::string& key) {
        auto it = py_.find(key);
        if (it == py_.end()) {
            failures_.push_back(key + ": MISSING from Python output");
            return 0;
        }
        return static_cast<unsigned long long>(it->second);
    }

    bool ok() const { return failures_.empty(); }

    std::string report() const {
        std::ostringstream o;
        o << "Python 镜像与 C 定义不一致（" << failures_.size() << " 处）：";
        for (const auto& f : failures_) o << "\n  - " << f;
        return o.str();
    }

private:
    const std::map<std::string, long long>& py_;
    std::vector<std::string> failures_;
};

/* 运行 `python3 <script>` 并把 stderr 落到 err_path；返回退出码。 */
int run_python(const std::string& script_path, const std::string& err_path,
               std::string& out) {
    std::string cmd = "python3 \"" + script_path + "\" 2>\""
                      + err_path + "\" ; echo __RC__=$?";
    FILE* f = ::popen(cmd.c_str(), "r");
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

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::ostringstream o;
    o << in.rdbuf();
    return o.str();
}

/* 解析 KEY=VALUE 行；非该形态的行忽略。 */
std::map<std::string, long long> parse_kv(const std::string& text) {
    std::map<std::string, long long> kv;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        /* 只接受 [A-Z0-9_]+ 作为键，避免把杂散输出当数据。 */
        bool key_ok = true;
        for (char ch : key) {
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')
                  || ch == '_')) {
                key_ok = false;
                break;
            }
        }
        if (!key_ok) continue;
        char* end = nullptr;
        const long long v = std::strtoll(line.c_str() + eq + 1, &end, 10);
        if (end == line.c_str() + eq + 1) continue;
        kv[key] = v;
    }
    return kv;
}

/* 取 KEY=VALUE 行的字符串值（用于 hex 串）。 */
std::string kv_str(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, key.size() + 1, key + "=") == 0) {
            return line.substr(key.size() + 1);
        }
    }
    return std::string();
}

/* 用真实 C 实现（irt_header_seal）按同一组参数构造 Header。
 * owner_pid 两侧都置 0（Python builder 不写该字段，属运行时字段）。 */
irt_header_t sealed_header(uint32_t max_points, uint32_t max_subscribers) {
    irt_header_t h;
    std::memset(&h, 0, sizeof(h));
    h.magic = IRT_MAGIC;
    h.version = IRT_SHM_VERSION;
    h.max_points = max_points;
    h.max_subscribers = max_subscribers;
    h.off_points = irt_layout_off_points();
    h.off_index = irt_layout_off_index(max_points);
    h.off_meta = irt_layout_off_meta(max_points);
    h.off_subs = irt_layout_off_subs(max_points);
    irt_header_seal(&h);
    return h;
}

std::string to_hex(const void* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    const unsigned char* p = static_cast<const unsigned char*>(data);
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 0xF];
    }
    return s;
}

}  // namespace

TEST(Mirror, PythonLayoutMatches) {
    ASSERT_STRNE(INDURTDB_TOOLS_DIR, "")
        << "INDURTDB_TOOLS_DIR 未定义：CMake 必须为该 target 注入 tools 目录";

    const std::string tools = INDURTDB_TOOLS_DIR;
    const std::string script_body = R"PYSCRIPT(
import os
import sys

TOOLS = "@TOOLS@"
sys.path.insert(0, os.path.join(TOOLS, "irt_migrate"))
sys.path.insert(0, os.path.join(TOOLS, "rtdb-monitor"))
sys.path.insert(0, os.path.join(TOOLS, "irt_diag"))

import irt_migrate.layout as L          # noqa: E402
import rtdb_client as P                # noqa: E402
import irt_diag._layout as D           # noqa: E402
import struct                          # noqa: E402
import zlib                            # noqa: E402
from irt_diag._layout import parse_v2_header  # noqa: E402

# C 侧 offsetof 真值由测试进程注入：Python 按 C 说的偏移去读自己构造的
# Header 字节，从而验证 **builder 硬编码的偏移**（而非仅验证 parser）。
C_OFF = @C_OFF@


def emit(k, v):
    print("%s=%d" % (k, int(v)))


N = 8
M = 2

# ---- 共享内存布局常量（irt_migrate.layout）----
for name in ("MAGIC", "SHM_VERSION_V1", "SHM_VERSION_V2", "SHM_VERSION_V3",
             "SHM_VERSION_CURRENT", "HEADER_V1_SIZE", "HEADER_V2_SIZE",
             "POINT_SIZE", "SUBS_ENTRY_SIZE", "INDEX_SLOT_SIZE",
             "META_SIZE_PER_POINT", "NAME_OFFSET", "NAME_LEN",
             "SOURCE_TS_OFFSET", "PARTITION_OFFSET", "INDEX_EMPTY",
             "INDEX_TOMBSTONE"):
    emit(name, getattr(L, name))

# ---- 派生几何（N=8, M=2）----
emit("BUCKETS", L.layout_buckets(N))
emit("INDEX_SIZE", L.layout_index_size(N))
emit("META_SIZE", L.layout_meta_size(N))
emit("OFF_POINTS", L.off_points_v2())
emit("OFF_INDEX", L.off_index_v2(N))
emit("OFF_META", L.off_meta_v2(N))
emit("OFF_SUBS", L.off_subs_v2(N))
emit("TOTAL_V2", L.total_size_v2(N, M))
emit("TOTAL_V1", L.total_size_v1(N, M))

# ---- irt_diag 侧镜像常量（T8 B1 同步项）----
for name in ("MAGIC", "SHM_VERSION_V1", "SHM_VERSION_CURRENT",
             "HEADER_V2_SIZE", "POINT_SIZE", "NAME_OFFSET", "NAME_LEN",
             "SOURCE_TS_OFFSET", "PARTITION_OFFSET", "RESERVED_OFFSET"):
    emit("DIAG_" + name, getattr(D, name))

# ---- rtdbd 协议常量与结构体尺寸（rtdb_client）----
emit("RTDBD_MAGIC", P.RTDBD_MAGIC)
emit("RTDBD_PROTO_VERSION", P.RTDBD_PROTO_VERSION)
emit("PARTITION_ALL", P.PARTITION_ALL)
for name in ("REQ_HDR", "RESP_HDR", "WRITE_REQ", "GET_REQ", "GET_RESP",
             "LIST_REQ", "POINT_INFO", "NOTIFY", "FIND_REQ", "FIND_RESP",
             "META_REQ", "META_PAYLOAD", "CREATE_REQ", "DELETE_REQ",
             "RENAME_REQ", "LOG_REQ", "LOG_ENTRY"):
    emit("PROTO_" + name, getattr(P, name).size)

# ---- Header 字段偏移 / CRC 区间：行为推导 ----
# 对 build_v2_header() 的每个字节翻转 1 bit，观察解析结果中哪个字段变化；
# 字段首次变化的字节位置即该字段（小端最低字节）的起始偏移。
hdr = L.build_v2_header(N, M)
emit("HDR_LEN", len(hdr))
base = parse_v2_header(hdr)


def first_change(field):
    for i in range(len(hdr)):
        b = bytearray(hdr)
        b[i] ^= 1
        try:
            v = parse_v2_header(bytes(b))[field]
        except Exception:
            continue
        if v != base[field]:
            return i
    return -1


for field, key in (("magic_ok", "HDR_MAGIC_OFF"),
                   ("version", "HDR_VERSION_OFF"),
                   ("max_points", "HDR_MAX_POINTS_OFF"),
                   ("max_subscribers", "HDR_MAX_SUBS_OFF"),
                   ("owner_pid", "HDR_OWNER_PID_OFF"),
                   ("crc_stored", "HDR_CRC_OFF"),
                   ("flags", "HDR_FLAGS_OFF"),
                   ("off_points", "HDR_OFF_POINTS"),
                   ("index_count", "HDR_INDEX_COUNT_OFF"),
                   ("scan_skipped", "HDR_SCAN_SKIPPED_OFF")):
    emit(key, first_change(field))

# CRC 覆盖集合：翻转该字节后 crc_calc 变化 => 该字节在 CRC 覆盖区间内。
cov = []
for i in range(len(hdr)):
    b = bytearray(hdr)
    b[i] ^= 1
    if parse_v2_header(bytes(b))["crc_calc"] != base["crc_calc"]:
        cov.append(i)

runs = []
if cov:
    lo = prev = cov[0]
    for i in cov[1:]:
        if i == prev + 1:
            prev = i
        else:
            runs.append((lo, prev + 1))
            lo = prev = i
    runs.append((lo, prev + 1))

emit("CRC_RUNS", len(runs))
for k, (a, b) in enumerate(runs):
    emit("CRC_RUN%d_LO" % k, a)
    emit("CRC_RUN%d_HI" % k, b)

# ---- Header builder 自检：按 C 的 offsetof 读回自己写的字节 ----
# 若 build_v2_header 把某字段写到错误偏移，这里读到的值就会偏离期望。
hdr = L.build_v2_header(N, M)
u32 = lambda off: struct.unpack_from("<I", hdr, off)[0]
emit("BUILD_MAGIC", u32(C_OFF["magic"]))
emit("BUILD_VERSION", u32(C_OFF["version"]))
emit("BUILD_MAX_POINTS", u32(C_OFF["max_points"]))
emit("BUILD_MAX_SUBS", u32(C_OFF["max_subscribers"]))
emit("BUILD_CRC", u32(C_OFF["crc32"]))
emit("BUILD_FLAGS", u32(C_OFF["flags"]))
emit("BUILD_OFF_POINTS", u32(C_OFF["off_points"]))
emit("BUILD_OFF_INDEX", u32(C_OFF["off_index"]))
emit("BUILD_OFF_META", u32(C_OFF["off_meta"]))
emit("BUILD_OFF_SUBS", u32(C_OFF["off_subs"]))
emit("BUILD_INDEX_COUNT", u32(C_OFF["index_count"]))
emit("BUILD_SCAN_SKIPPED", u32(C_OFF["scan_skipped"]))

print("BUILD_HDR_HEX=%s" % hdr.hex())

# CRC 必须覆盖 C 侧的 [0:16] + [48:72] 两段，且不覆盖其它字节。
C_CRC = (zlib.crc32(hdr[0:16] + hdr[48:72]) & 0xFFFFFFFF)
emit("BUILD_CRC_OK",
     1 if C_CRC == struct.unpack_from("<I", hdr, C_OFF["crc32"])[0] else 0)

# ---- irt_diag 的 parser 必须与 irt_migrate 的 builder 互认 ----
# 同一段 Header 由两个独立实现分别处理：迁移工具写、诊断工具读。
d = parse_v2_header(hdr)
emit("DIAG_PARSES_MIGRATE_HDR",
     1 if (d["magic_ok"] and d["version"] == L.SHM_VERSION_CURRENT
           and d["crc_ok"] and d["max_points"] == N
           and d["max_subscribers"] == M) else 0)
emit("DIAG_HDR_CRC", d["crc_stored"])
emit("DIAG_HDR_MAX_POINTS", d["max_points"])
emit("DIAG_HDR_OFF_INDEX", d["off_index"])
emit("DIAG_HDR_SCAN_SKIPPED", d["scan_skipped"])
)PYSCRIPT";

    std::string script = script_body;
    const std::string marker = "@TOOLS@";
    const auto at = script.find(marker);
    ASSERT_NE(at, std::string::npos) << "内部错误：脚本缺少 TOOLS 占位符";
    script.replace(at, marker.size(), tools);

    /* 把 C 侧 offsetof 真值以 Python dict 字面量注入，供 Python 读回自建
     * Header —— 这样 builder 硬编码偏移也受 C 真值约束。 */
    std::ostringstream off;
    off << "{"
        << "'magic': " << offsetof(irt_header_t, magic) << ", "
        << "'version': " << offsetof(irt_header_t, version) << ", "
        << "'max_points': " << offsetof(irt_header_t, max_points) << ", "
        << "'max_subscribers': " << offsetof(irt_header_t, max_subscribers)
        << ", "
        << "'crc32': " << offsetof(irt_header_t, crc32) << ", "
        << "'flags': " << offsetof(irt_header_t, flags) << ", "
        << "'off_points': " << offsetof(irt_header_t, off_points) << ", "
        << "'off_index': " << offsetof(irt_header_t, off_index) << ", "
        << "'off_meta': " << offsetof(irt_header_t, off_meta) << ", "
        << "'off_subs': " << offsetof(irt_header_t, off_subs) << ", "
        << "'index_count': " << offsetof(irt_header_t, index_count) << ", "
        << "'scan_skipped': " << offsetof(irt_header_t, scan_skipped)
        << "}";
    const std::string off_marker = "@C_OFF@";
    const auto oat = script.find(off_marker);
    ASSERT_NE(oat, std::string::npos) << "内部错误：脚本缺少 C_OFF 占位符";
    script.replace(oat, off_marker.size(), off.str());

    char script_tmpl[] = "/tmp/irt_mirror_XXXXXX";
    const int sfd = ::mkstemp(script_tmpl);
    ASSERT_GE(sfd, 0) << "mkstemp 失败（脚本）";
    {
        std::ofstream out(script_tmpl);
        out << script;
        ASSERT_TRUE(out.good()) << "写入临时脚本失败";
    }
    ::close(sfd);

    char err_tmpl[] = "/tmp/irt_mirror_err_XXXXXX";
    const int efd = ::mkstemp(err_tmpl);
    ASSERT_GE(efd, 0) << "mkstemp 失败（stderr）";
    ::close(efd);

    std::string out;
    const int rc = run_python(script_tmpl, err_tmpl, out);
    const std::string err = read_file(err_tmpl);
    ::unlink(script_tmpl);
    ::unlink(err_tmpl);

    ASSERT_EQ(rc, 0) << "python3 执行失败（rc=" << rc
                     << "）；需 python3 且 tools 下模块可导入。stderr:\n"
                     << err << "\nstdout:\n" << out;

    const std::map<std::string, long long> py = parse_kv(out);
    ASSERT_FALSE(py.empty()) << "Python 未输出任何 KEY=VALUE。stdout:\n" << out;

    MirrorCheck m(py);

    /* ================= 共享内存布局常量 ================= */
    m.eq("MAGIC", IRT_MAGIC);
    m.eq("SHM_VERSION_V3", IRT_SHM_VERSION);
    m.eq("SHM_VERSION_CURRENT", IRT_SHM_VERSION);
    m.eq("HEADER_V2_SIZE", sizeof(irt_header_t));
    m.eq("POINT_SIZE", sizeof(indurtdb_point_t));
    m.eq("SUBS_ENTRY_SIZE", sizeof(irt_subscriber_entry_t));
    m.eq("INDEX_SLOT_SIZE", sizeof(irt_index_slot_t));
    m.eq_raw("IRT_INDEX_SLOT_SIZE == sizeof(irt_index_slot_t)",
             sizeof(irt_index_slot_t), IRT_INDEX_SLOT_SIZE);
    m.eq("META_SIZE_PER_POINT", sizeof(indurtdb_meta_t));
    m.eq("INDEX_EMPTY", IRT_INDEX_EMPTY);
    m.eq("INDEX_TOMBSTONE", IRT_INDEX_TOMBSTONE);

    /* v1 Header 结构体已从代码移除，但其 44B 负载（magic..stats）与当前
     * Header 同偏移 —— 用 offsetof(crc32) 作为该负载的结束位置，再加 20B
     * 历史保留尾，钉住迁移工具依赖的 64B。C 侧若在 v1 前缀插入字段，
     * crc32 偏移变化即会打破此等式。 */
    m.eq("HEADER_V1_SIZE", offsetof(irt_header_t, crc32) + 20);

    /* ================= 点位记录：常量 + 派生不变式链 =================
     * Python 侧没有 value/timestamp/type/quality/unit/access/padding/reserved
     * 常量。用「相邻字段首尾相接」的链式等式把缺失偏移全部钉死：
     * 任一 C 侧或 Python 侧漂移都会破坏至少一条等式。 */
    m.eq("NAME_OFFSET", offsetof(indurtdb_point_t, name));
    m.eq("NAME_LEN", sizeof(((indurtdb_point_t*)nullptr)->name));
    m.eq("SOURCE_TS_OFFSET", offsetof(indurtdb_point_t, source_timestamp_ns));
    m.eq("PARTITION_OFFSET", offsetof(indurtdb_point_t, partition));

    /* name[45..109) 之后紧跟 padding[3]，即 source_timestamp_ns - 3。 */
    m.eq_raw("NAME_OFFSET + NAME_LEN (padding start)",
             offsetof(indurtdb_point_t, source_timestamp_ns) - 3,
             m.v("NAME_OFFSET") + m.v("NAME_LEN"));
    /* source_timestamp_ns(8B) 之后即 partition。 */
    m.eq_raw("SOURCE_TS_OFFSET + 8 (partition start)",
             offsetof(indurtdb_point_t, partition),
             m.v("SOURCE_TS_OFFSET") + 8);
    /* partition(4B) 之后即 reserved[4]。 */
    m.eq_raw("PARTITION_OFFSET + 4 (reserved start)",
             offsetof(indurtdb_point_t, reserved),
             m.v("PARTITION_OFFSET") + 4);
    /* partition + 4B reserved == 记录末尾。 */
    m.eq_raw("PARTITION_OFFSET + 8 (point end)",
             sizeof(indurtdb_point_t),
             m.v("PARTITION_OFFSET") + 8);

    /* ================= 派生几何（N=8, M=2）================= */
    const uint32_t N = 8;
    const uint32_t M = 2;
    m.eq("BUCKETS", irt_layout_buckets(N));
    m.eq("INDEX_SIZE", irt_layout_index_size(N));
    m.eq("META_SIZE", irt_layout_meta_size(N));
    m.eq("OFF_POINTS", irt_layout_off_points());
    m.eq("OFF_INDEX", irt_layout_off_index(N));
    m.eq("OFF_META", irt_layout_off_meta(N));
    m.eq("OFF_SUBS", irt_layout_off_subs(N));
    m.eq("TOTAL_V2", irt_shm_total_size(N, M));
    m.eq_raw("TOTAL_V1",
             offsetof(irt_header_t, crc32) + 20
                 + static_cast<size_t>(N) * sizeof(indurtdb_point_t)
                 + static_cast<size_t>(M) * sizeof(irt_subscriber_entry_t),
             m.v("TOTAL_V1"));

    /* ================= irt_diag 镜像常量（T8 B1 同步）================= */
    m.eq("DIAG_MAGIC", IRT_MAGIC);
    m.eq("DIAG_SHM_VERSION_V1", 1);
    m.eq("DIAG_SHM_VERSION_CURRENT", IRT_SHM_VERSION);
    m.eq("DIAG_HEADER_V2_SIZE", sizeof(irt_header_t));
    m.eq("DIAG_POINT_SIZE", sizeof(indurtdb_point_t));
    m.eq("DIAG_NAME_OFFSET", offsetof(indurtdb_point_t, name));
    m.eq("DIAG_NAME_LEN", sizeof(((indurtdb_point_t*)nullptr)->name));
    m.eq("DIAG_SOURCE_TS_OFFSET",
         offsetof(indurtdb_point_t, source_timestamp_ns));
    m.eq("DIAG_PARTITION_OFFSET", offsetof(indurtdb_point_t, partition));
    m.eq("DIAG_RESERVED_OFFSET", offsetof(indurtdb_point_t, reserved));

    /* ================= Header 字段偏移（Python 硬编码值）================= */
    m.eq("HDR_LEN", sizeof(irt_header_t));
    m.eq("HDR_MAGIC_OFF", offsetof(irt_header_t, magic));
    m.eq("HDR_VERSION_OFF", offsetof(irt_header_t, version));
    m.eq("HDR_MAX_POINTS_OFF", offsetof(irt_header_t, max_points));
    m.eq("HDR_MAX_SUBS_OFF", offsetof(irt_header_t, max_subscribers));
    m.eq("HDR_OWNER_PID_OFF", offsetof(irt_header_t, owner_pid));
    m.eq("HDR_CRC_OFF", offsetof(irt_header_t, crc32));
    m.eq("HDR_FLAGS_OFF", offsetof(irt_header_t, flags));
    m.eq("HDR_OFF_POINTS", offsetof(irt_header_t, off_points));
    m.eq("HDR_INDEX_COUNT_OFF", offsetof(irt_header_t, index_count));
    m.eq("HDR_SCAN_SKIPPED_OFF", offsetof(irt_header_t, scan_skipped));

    /* CRC 覆盖区间必须是 [magic, write_seq) + [flags, index_count)：
     * Python 行为推导出的两段区间须与 C offsetof 完全吻合。 */
    m.eq("CRC_RUNS", 2);
    m.eq("CRC_RUN0_LO", offsetof(irt_header_t, magic));
    m.eq("CRC_RUN0_HI", offsetof(irt_header_t, magic) + 16);
    m.eq("CRC_RUN1_LO", offsetof(irt_header_t, flags));
    m.eq("CRC_RUN1_HI", offsetof(irt_header_t, flags) + 24);

    /* ================= Header builder：按 C 偏移读回自建字节 =================
     * 上面只证明了 parser 的偏移；这里证明 builder 写的位置也对。 */
    m.eq("BUILD_MAGIC", IRT_MAGIC);
    m.eq("BUILD_VERSION", IRT_SHM_VERSION);
    m.eq("BUILD_MAX_POINTS", N);
    m.eq("BUILD_MAX_SUBS", M);
    m.eq("BUILD_FLAGS", 0);
    m.eq("BUILD_OFF_POINTS", irt_layout_off_points());
    m.eq("BUILD_OFF_INDEX", irt_layout_off_index(N));
    m.eq("BUILD_OFF_META", irt_layout_off_meta(N));
    m.eq("BUILD_OFF_SUBS", irt_layout_off_subs(N));
    m.eq("BUILD_INDEX_COUNT", 0);
    m.eq("BUILD_SCAN_SKIPPED", 0);
    /* CRC 与整段字节：由真实 C 实现 irt_header_seal() 生成同参数的
     * Header，再与 Python builder 的 128 字节逐字节比对。 */
    const irt_header_t c_hdr = sealed_header(N, M);
    m.eq_raw("BUILD_CRC (irt_header_seal over C ranges)",
             c_hdr.crc32, m.v("BUILD_CRC"));
    m.eq("BUILD_CRC_OK", 1);
    EXPECT_EQ(to_hex(&c_hdr, sizeof(c_hdr)), kv_str(out, "BUILD_HDR_HEX"))
        << "Python build_v2_header 与 C irt_header_seal 生成的 Header "
           "字节不一致（逐字节比对）";

    /* ================= irt_diag parser 与 irt_migrate builder 互认 =========
     * 两个独立 Python 实现（诊断读 / 迁移写）必须能互相解释同一段字节。 */
    m.eq("DIAG_PARSES_MIGRATE_HDR", 1);
    m.eq_raw("DIAG_HDR_CRC == BUILT CRC",
             m.v("BUILD_CRC"), m.v("DIAG_HDR_CRC"));
    m.eq("DIAG_HDR_MAX_POINTS", N);
    m.eq("DIAG_HDR_OFF_INDEX", irt_layout_off_index(N));
    m.eq("DIAG_HDR_SCAN_SKIPPED", 0);

    /* ================= rtdbd 协议 ================= */
    m.eq("RTDBD_MAGIC", RTDBD_MAGIC);
    m.eq("RTDBD_PROTO_VERSION", RTDBD_PROTO_VERSION);
    m.eq("PARTITION_ALL", RTDBD_PARTITION_ALL);
    m.eq("PROTO_REQ_HDR", sizeof(rtdbd_req_hdr_t));
    m.eq("PROTO_RESP_HDR", sizeof(rtdbd_resp_hdr_t));
    m.eq("PROTO_WRITE_REQ", sizeof(rtdbd_write_req_t));
    m.eq("PROTO_GET_REQ", sizeof(rtdbd_get_req_t));
    m.eq("PROTO_GET_RESP", sizeof(rtdbd_get_resp_t));
    m.eq("PROTO_LIST_REQ", sizeof(rtdbd_list_req_t));
    m.eq("PROTO_POINT_INFO", sizeof(rtdbd_point_info_t));
    m.eq("PROTO_NOTIFY", sizeof(rtdbd_notify_t));
    m.eq("PROTO_FIND_REQ", sizeof(rtdbd_find_req_t));
    m.eq("PROTO_FIND_RESP", sizeof(rtdbd_find_resp_t));
    m.eq("PROTO_META_REQ", sizeof(rtdbd_meta_req_t));
    m.eq("PROTO_META_PAYLOAD", sizeof(rtdbd_meta_payload_t));
    m.eq("PROTO_CREATE_REQ", sizeof(rtdbd_create_req_t));
    m.eq("PROTO_DELETE_REQ", sizeof(rtdbd_delete_req_t));
    m.eq("PROTO_RENAME_REQ", sizeof(rtdbd_rename_req_t));
    m.eq("PROTO_LOG_REQ", sizeof(rtdbd_log_req_t));
    m.eq("PROTO_LOG_ENTRY", sizeof(rtdbd_log_entry_t));

    EXPECT_TRUE(m.ok()) << m.report();
    if (m.ok()) {
        std::cout << "OK Mirror.PythonLayoutMatches "
                     "(shm point/header/index + protocol structs)"
                  << std::endl;
    }
}

TEST(Mirror, V311ProtocolLayout)
{
    EXPECT_EQ(RTDBD_OP_BATCH_READ, 19u);
    EXPECT_EQ(RTDBD_OP_BATCH_WRITE, 20u);
    EXPECT_EQ(RTDBD_OP_CAPS, 21u);
    EXPECT_EQ(RTDBD_OP_SNAPSHOT_DONE, 22u);
    EXPECT_EQ(RTDBD_ST_NO_MEM, 5u);
    EXPECT_EQ(RTDBD_SUB_MAX_PER_CONN, 10000u);
    EXPECT_EQ(RTDBD_BATCH_MAX, 1024u);
    EXPECT_EQ(RTDBD_NOTIFY_F_SNAPSHOT, 0x01u);
    EXPECT_EQ(RTDBD_CAPS_F_BATCH_READ, 0x01u);
    EXPECT_EQ(RTDBD_CAPS_F_BATCH_WRITE, 0x02u);
    EXPECT_EQ(RTDBD_CAPS_F_SNAPSHOT_DONE, 0x04u);
    EXPECT_EQ(RTDBD_SNAPSHOT_DONE_F_COMPLETE, 0x01u);
    EXPECT_EQ(sizeof(rtdbd_caps_t), 32u);
    EXPECT_EQ(sizeof(rtdbd_batch_read_item_t), 72u);
    EXPECT_EQ(sizeof(rtdbd_batch_write_item_t), 8u);
    EXPECT_EQ(sizeof(rtdbd_snapshot_done_t), 8u);
}
