#include "common.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace relay {

uint64_t now_us() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 10;   // FILETIME (100ns, 1601) -> us
}

uint64_t mono_ns() {
    using clock = std::chrono::steady_clock;
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        clock::now().time_since_epoch())
                        .count());
}

static void split_time(uint64_t us, SYSTEMTIME* st, unsigned* ms) {
    uint64_t ft100 = us * 10 + 116444736000000000ULL;
    FILETIME ft;
    ft.dwLowDateTime = uint32_t(ft100 & 0xFFFFFFFFu);
    ft.dwHighDateTime = uint32_t(ft100 >> 32);
    FILETIME lf;
    FileTimeToLocalFileTime(&ft, &lf);
    FileTimeToSystemTime(&lf, st);
    *ms = unsigned((us / 1000) % 1000);
}

std::string stamp_compact(uint64_t us) {
    SYSTEMTIME st;
    unsigned ms;
    split_time(us, &st, &ms);
    char b[32];
    std::snprintf(b, sizeof(b), "%04u%02u%02u_%02u%02u%02u", st.wYear,
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
}

std::string stamp_human(uint64_t us) {
    SYSTEMTIME st;
    unsigned ms;
    split_time(us, &st, &ms);
    char b[40];
    std::snprintf(b, sizeof(b), "%04u-%02u-%02u %02u:%02u:%02u.%03u", st.wYear,
                  st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, ms);
    return b;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            tbl[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = tbl[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint32_t crc32(const std::string& s) {
    return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)),
               std::istreambuf_iterator<char>());
    return true;
}

// Write to a sibling temp file, flush it to the platter, then rename over the
// target. A crash in the middle leaves either the old file or the new one --
// never a half-written client.xml the game refuses to start with.
bool write_file_atomic(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tosrelay.tmp";
    HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    BOOL ok = data.empty() ? TRUE
                           : WriteFile(h, data.data(), DWORD(data.size()),
                                       &wrote, nullptr);
    if (ok) ok = FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || (!data.empty() && wrote != data.size())) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

bool file_exists(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool make_dirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '\\' || path[i] == '/') {
            if (cur.size() > 1 && cur.back() != ':') CreateDirectoryA(cur.c_str(), nullptr);
        }
        if (i < path.size()) cur.push_back(path[i]);
    }
    DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

std::string base_name(const std::string& path) {
    size_t p = path.find_last_of("\\/");
    return p == std::string::npos ? path : path.substr(p + 1);
}

int set_xml_attr(std::string& xml, const std::string& attr,
                 const std::string& value) {
    std::string needle = attr + "=\"";
    int n = 0;
    size_t at = 0;
    while ((at = xml.find(needle, at)) != std::string::npos) {
        size_t vs = at + needle.size();
        size_t ve = xml.find('"', vs);
        if (ve == std::string::npos) break;
        xml.replace(vs, ve - vs, value);
        at = vs + value.size() + 1;
        ++n;
    }
    return n;
}

std::string get_xml_attr(const std::string& xml, const std::string& attr) {
    std::string needle = attr + "=\"";
    size_t at = xml.find(needle);
    if (at == std::string::npos) return std::string();
    size_t vs = at + needle.size();
    size_t ve = xml.find('"', vs);
    if (ve == std::string::npos) return std::string();
    return xml.substr(vs, ve - vs);
}

std::string human_bytes(uint64_t n) {
    const char* u[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(n);
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char b[32];
    std::snprintf(b, sizeof(b), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
    return b;
}

}  // namespace relay
