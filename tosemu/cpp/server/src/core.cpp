#include "core.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>

namespace tos {

bool read_file(const std::string& path, Bytes& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(size_t(n < 0 ? 0 : n));
    if (!out.empty()) f.read(reinterpret_cast<char*>(out.data()), n);
    return true;
}

bool write_file(const std::string& path, const uint8_t* p, size_t n) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    if (n) f.write(reinterpret_cast<const char*>(p), std::streamsize(n));
    return bool(f);
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return std::string(s.substr(a, b - a));
}

std::string cstr(const uint8_t* p, size_t max) {
    size_t n = 0;
    while (n < max && p[n]) ++n;
    return std::string(reinterpret_cast<const char*>(p), n);
}

std::string hex(const uint8_t* p, size_t n, size_t cap) {
    static const char* d = "0123456789abcdef";
    std::string out;
    size_t k = std::min(n, cap);
    out.reserve(k * 2 + 8);
    for (size_t i = 0; i < k; ++i) {
        out += d[p[i] >> 4];
        out += d[p[i] & 15];
    }
    if (k < n) out += "..";
    return out;
}

// ---- logging -----------------------------------------------------------

namespace {

std::mutex g_log_mu;
LogLevel g_min = LogLevel::Info;

const char* level_tag(LogLevel lv) {
    switch (lv) {
        case LogLevel::Debug: return "dbg";
        case LogLevel::Info:  return "   ";
        case LogLevel::Warn:  return "WRN";
        default:              return "ERR";
    }
}

std::string clock_string() {
    using namespace std::chrono;
    auto tp = system_clock::now();
    auto ms = duration_cast<milliseconds>(tp.time_since_epoch()) % 1000;
    std::time_t t = system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min,
                  tm.tm_sec, int(ms.count()));
    return buf;
}

}  // namespace

void log_set_min_level(LogLevel lv) { g_min = lv; }

void log_line(LogLevel lv, std::string_view tag, std::string_view msg) {
    if (lv < g_min) return;
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::printf("[%s] %s %-9.*s %.*s\n", clock_string().c_str(), level_tag(lv),
                int(tag.size()), tag.data(), int(msg.size()), msg.data());
    std::fflush(stdout);
}

double now_sec() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

uint64_t now_filetime() {
    using namespace std::chrono;
    // FILETIME counts 100ns ticks from 1601-01-01; 11644473600s to the epoch.
    auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch())
                  .count();
    return uint64_t(us) * 10ULL + 116444736000000000ULL;
}

std::string utc_string(int delta_sec) {
    using namespace std::chrono;
    std::time_t t = system_clock::to_time_t(system_clock::now()) + delta_sec;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace tos
