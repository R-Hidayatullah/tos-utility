#include "stats.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "common.h"
#include "dump.h"
#include "guard.h"
#include "log.h"

namespace relay {

void Stats::start() {
    start_us_ = now_us();
    start_mono_ = mono_ns();
    last_mono_ = start_mono_;
}

void Stats::packet(int dir, uint16_t op, const std::string& name, uint32_t len,
                   uint32_t wire, int declared, uint8_t link,
                   bool checksum_bad) {
    int d = dir & 1;
    pkt_[d].fetch_add(1);
    bytes_[d].fetch_add(len);
    wire_[d].fetch_add(wire);
    if (checksum_bad) bad_.fetch_add(1);

    std::lock_guard<std::mutex> lk(m_);
    auto& s = ops_[{d, op}];
    bool first = s.count == 0;
    if (first) {
        s.name = name;
        s.link = link;
    }
    s.count++;
    s.bytes += len;
    s.last_us = now_us();
    s.declared = uint16_t(declared > 0 ? declared : 0);
    if (first) {
        // One line the first time each opcode appears -- that is the useful
        // signal. Logging every packet would bury it at a few hundred a second.
        static const char* kLink[] = {"?", "barrack", "zone", "social"};
        LOGI("%s %-34s op=%-5u declared=%-5d link=%s%s", d ? "s2c" : "c2s",
             name.c_str(), op, declared, kLink[link & 3],
             checksum_bad ? "  chk=BAD" : "");
    }
}

void Stats::unframed(uint16_t op, uint32_t len, bool inferred) {
    unframed_bytes_.fetch_add(len);
    unframed_recs_.fetch_add(1);

    std::lock_guard<std::mutex> lk(m_);
    auto& u = unknown_[op];
    bool first = u.count == 0;
    u.count++;
    if (len < u.min_len) u.min_len = len;
    if (len > u.max_len) u.max_len = len;
    u.length_inferred = u.length_inferred || inferred;
    if (first)
        LOGW("s2c opcode %u is not in packet_opcodes.csv -- captured %u byte(s) "
             "unframed%s", op, len, inferred ? ", length inferred" : "");
}

void Stats::unknown_c2s(uint16_t op, uint32_t len) {
    std::lock_guard<std::mutex> lk(m_);
    auto& u = unknown_[op];
    bool first = u.count == 0;
    u.count++;
    u.from_client = true;
    if (len < u.min_len) u.min_len = len;
    if (len > u.max_len) u.max_len = len;
    if (first)
        LOGW("c2s opcode %u is not in packet_opcodes.csv -- captured whole "
             "(%u bytes) from its own frame length", op, len);
}

std::vector<std::pair<uint16_t, Stats::Unknown>> Stats::unknowns() const {
    std::lock_guard<std::mutex> lk(m_);
    return {unknown_.begin(), unknown_.end()};
}

void Stats::listener(uint16_t local, const std::string& upstream) {
    char b[128];
    std::snprintf(b, sizeof(b), "%5u -> %s", local, upstream.c_str());
    std::lock_guard<std::mutex> lk(m_);
    listeners_.push_back(b);
}

uint64_t Stats::distinct() const {
    std::lock_guard<std::mutex> lk(m_);
    return ops_.size();
}

std::vector<std::pair<int, OpStat>> Stats::top(size_t n) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::pair<int, OpStat>> v;
    v.reserve(ops_.size());
    for (const auto& kv : ops_) v.push_back({kv.first.first, kv.second});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second.count > b.second.count;
    });
    if (v.size() > n) v.resize(n);
    return v;
}

std::vector<std::string> Stats::listeners() const {
    std::lock_guard<std::mutex> lk(m_);
    return listeners_;
}

Stats::Rate Stats::sample() {
    uint64_t mono = mono_ns();
    uint64_t p = pkt_[0].load() + pkt_[1].load();
    uint64_t b = bytes_[0].load() + bytes_[1].load();
    std::lock_guard<std::mutex> lk(m_);
    double dt = double(mono - last_mono_) / 1e9;
    Rate r{0, 0};
    if (dt > 0.001) {
        r.pps = double(p - last_pkt_) / dt;
        r.bps = double(b - last_bytes_) / dt;
    }
    last_mono_ = mono;
    last_pkt_ = p;
    last_bytes_ = b;
    return r;
}

// ------------------------------------------------------------- dashboard

static bool enable_vt() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;   // redirected to a file
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
}

static int console_rows() {
    CONSOLE_SCREEN_BUFFER_INFO ci;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci))
        return 30;
    return ci.srWindow.Bottom - ci.srWindow.Top + 1;
}

void Dashboard::start(Stats* s, Dump* d, Guard* g, const std::string& title) {
    s_ = s;
    d_ = d;
    g_ = g;
    title_ = title;
    vt_ = enable_vt();
    if (vt_) {
        int rows = console_rows();
        if (height_ > rows - 4) height_ = rows > 8 ? rows - 4 : 4;
        std::ostringstream o;
        o << "\x1b[2J\x1b[H";                       // clear
        o << "\x1b[" << (height_ + 1) << ';' << rows << 'r';   // scroll region
        o << "\x1b[" << (height_ + 1) << ";1H";     // park the cursor in it
        g_log.raw(o.str());
    }
    run_ = true;
    th_ = std::thread(&Dashboard::loop, this);
}

void Dashboard::stop() {
    if (!run_.exchange(false)) return;
    if (th_.joinable()) th_.join();
    if (vt_) {
        std::ostringstream o;
        // Release the scroll region and put the final frame in the scrollback
        // so the summary survives after the process is gone.
        o << "\x1b[r\x1b[" << (height_ + 1) << ";1H";
        g_log.raw(o.str());
    }
}

void Dashboard::loop() {
    if (!stdout_is_console()) return;         // redirected: the log has it all
    int tick = 0;
    while (run_.load()) {
        if (vt_ || tick % 20 == 0)            // every ~5s without VT
            g_log.raw(frame());
        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    g_log.raw(frame());
}

std::string Dashboard::frame() {
    Stats::Rate r = s_->sample();
    uint64_t up_s = (now_us() - s_->start_us()) / 1000000;

    std::vector<std::string> L;
    char b[256];

    std::snprintf(b, sizeof(b), "  %s   up %02llu:%02llu:%02llu   %s",
                  title_.c_str(), (unsigned long long)(up_s / 3600),
                  (unsigned long long)((up_s / 60) % 60),
                  (unsigned long long)(up_s % 60),
                  stamp_human(now_us()).c_str());
    L.push_back(b);

    std::snprintf(b, sizeof(b),
                  "  conns  %llu active / %llu total     packets  %llu c2s  "
                  "%llu s2c     %.0f pkt/s  %s/s",
                  (unsigned long long)s_->conns_active(),
                  (unsigned long long)s_->conns_total(),
                  (unsigned long long)s_->packets(0),
                  (unsigned long long)s_->packets(1), r.pps,
                  human_bytes(uint64_t(r.bps)).c_str());
    L.push_back(b);

    std::snprintf(b, sizeof(b),
                  "  bytes  %s c2s  %s s2c  (wire %s / %s)     opcodes %llu     "
                  "chk BAD %llu  resync %llu  unframed %llu  http %llu",
                  human_bytes(s_->bytes(0)).c_str(),
                  human_bytes(s_->bytes(1)).c_str(),
                  human_bytes(s_->wire(0)).c_str(),
                  human_bytes(s_->wire(1)).c_str(),
                  (unsigned long long)s_->distinct(),
                  (unsigned long long)s_->bad_checksum(),
                  (unsigned long long)s_->resyncs(),
                  (unsigned long long)s_->unframed_records(),
                  (unsigned long long)s_->http_hits());
    L.push_back(b);

    auto unk = s_->unknowns();
    if (!unk.empty()) {
        std::string line = "  new    ";     // opcodes the table does not have
        for (size_t i = 0; i < unk.size(); ++i) {
            char t[48];
            std::snprintf(t, sizeof(t), "%u(%s%u)", unk[i].first,
                          unk[i].second.from_client ? "c" : "s",
                          unk[i].second.max_len);
            if (line.size() + std::strlen(t) > 150) {
                line += "...";
                break;
            }
            line += t;
            if (i + 1 < unk.size()) line += " ";
        }
        L.push_back(line);
    }

    if (d_) {
        std::snprintf(b, sizeof(b),
                      "  dump   %s   %llu records  %s%s",
                      base_name(d_->path()).c_str(),
                      (unsigned long long)d_->records(),
                      human_bytes(d_->bytes()).c_str(),
                      d_->dropped() ? "  DROPPED!" : "");
        L.push_back(b);
    }

    if (g_) {
        auto e = g_->entries();
        if (e.empty()) {
            L.push_back("  files  none patched - game config is original");
        } else {
            std::snprintf(b, sizeof(b),
                          "  files  %u PATCHED (restored on exit; journal %s)",
                          unsigned(e.size()),
                          base_name(g_->journal_path()).c_str());
            L.push_back(b);
            for (size_t i = 0; i < e.size() && i < 3; ++i) {
                // Tail of the path only: the full one is in the log, and a
                // truncated absolute path tells you nothing at a glance.
                const std::string& t = e[i].target;
                size_t cut = t.find_last_of("\\/");
                if (cut != std::string::npos && cut > 0)
                    cut = t.find_last_of("\\/", cut - 1);
                L.push_back("           ..." +
                            (cut == std::string::npos ? t : t.substr(cut)));
            }
        }
    }

    auto ls = s_->listeners();
    std::string line = "  listen ";
    for (size_t i = 0; i < ls.size(); ++i) {
        line += ls[i];
        if (i + 1 < ls.size()) line += "   ";
        if (line.size() > 150) { line += " ..."; break; }
    }
    L.push_back(line);

    L.push_back("  ---- top packets ---------------------------------------"
                "------------------------------");
    for (const auto& kv : s_->top(size_t(height_) > L.size() + 1
                                      ? size_t(height_) - L.size() - 1
                                      : 0)) {
        std::snprintf(b, sizeof(b), "  %-4s %-38s %8llu  %10s",
                      kv.first ? "s2c" : "c2s", kv.second.name.c_str(),
                      (unsigned long long)kv.second.count,
                      human_bytes(kv.second.bytes).c_str());
        L.push_back(b);
    }

    std::ostringstream o;
    if (vt_) {
        o << "\x1b[s\x1b[H";                       // save cursor, go home
        for (int i = 0; i < height_; ++i) {
            o << "\x1b[" << (i + 1) << ";1H\x1b[2K";
            if (i < int(L.size())) {
                if (i == 0) o << "\x1b[1;96m" << L[i] << "\x1b[0m";
                else o << L[i];
            }
        }
        o << "\x1b[u";                             // back to the scroll area
    } else {
        o << "\n";
        for (const auto& s : L) o << s << "\n";
        o << "\n";
    }
    return o.str();
}

}  // namespace relay
