#include "log.h"

#include <windows.h>

#include <cstdarg>

#include "common.h"

namespace relay {

Log g_log;

// Whether stdout is a real console. When it is not -- piped to a file, or to
// another process -- colour codes are noise and the dashboard is worse than
// noise, so both are suppressed and the log file carries everything.
bool stdout_is_console() {
    static const bool v = [] {
        DWORD mode = 0;
        return GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode) != 0;
    }();
    return v;
}

static const char* lv_name(Lv lv) {
    switch (lv) {
        case Lv::Dbg:  return "DBG";
        case Lv::Info: return "INF";
        case Lv::Warn: return "WRN";
        default:       return "ERR";
    }
}

// 4-bit console colours; the dashboard leaves the scroll area to us.
static const char* lv_color(Lv lv) {
    switch (lv) {
        case Lv::Dbg:  return "\x1b[90m";
        case Lv::Info: return "\x1b[0m";
        case Lv::Warn: return "\x1b[33m";
        default:       return "\x1b[91m";
    }
}

bool Log::open(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_);
    f_ = std::fopen(path.c_str(), "w");
    if (!f_) return false;
    path_ = path;
    ring_.assign(200, std::string());
    return true;
}

void Log::close() {
    std::lock_guard<std::mutex> lk(m_);
    if (f_) { std::fflush(f_); std::fclose(f_); f_ = nullptr; }
}

void Log::write(Lv lv, const std::string& msg) {
    std::string line = stamp_human(now_us());
    line += " [";
    line += lv_name(lv);
    line += "] ";
    line += msg;

    std::lock_guard<std::mutex> lk(m_);
    if (f_) {
        std::fputs(line.c_str(), f_);
        std::fputc('\n', f_);
        std::fflush(f_);      // survive a kill: the restore state is in here
    }
    if (!ring_.empty()) {
        ring_[ring_at_ % ring_.size()] = line;
        ++ring_at_;
    }
    if (stdout_is_console()) {
        // 12 chars in is past the date, which the dashboard already shows.
        std::fputs(lv_color(lv), stdout);
        std::fputs(line.c_str() + 11, stdout);
        std::fputs("\x1b[0m\n", stdout);
    } else {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
    }
    std::fflush(stdout);
}

void Log::logf(Lv lv, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(lv, buf);
}

void Log::raw(const std::string& s) {
    std::lock_guard<std::mutex> lk(m_);
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fflush(stdout);
}

std::vector<std::string> Log::tail(size_t n) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::string> out;
    if (ring_.empty()) return out;
    size_t have = ring_at_ < ring_.size() ? ring_at_ : ring_.size();
    if (n > have) n = have;
    for (size_t i = have - n; i < have; ++i)
        out.push_back(ring_[(ring_at_ - have + i) % ring_.size()]);
    return out;
}

}  // namespace relay
