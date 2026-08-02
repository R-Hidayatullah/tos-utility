// Thread-safe logging to a timestamped file and to the console.
//
// The log file is the record of what happened to the user's game files. If the
// relay is killed before it can restore them, the last lines of this file are
// the only thing that says which files are still patched and where the backups
// are -- so every line is flushed as it is written, and the file is opened
// before the first patch and closed after the last restore.
#pragma once

#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace relay {

enum class Lv { Dbg, Info, Warn, Err };

class Log {
public:
    // `path` is created and truncated. Console output goes to the scroll area
    // below the dashboard once Dashboard::begin() has reserved one.
    bool open(const std::string& path);
    void close();

    void write(Lv lv, const std::string& msg);
    void logf(Lv lv, const char* fmt, ...);

    // Console-only, written under the same lock as log lines so a dashboard
    // repaint can never land in the middle of one.
    void raw(const std::string& s);

    // Last N lines, for the dashboard's tail pane.
    std::vector<std::string> tail(size_t n) const;
    const std::string& path() const { return path_; }

private:
    mutable std::mutex m_;
    std::FILE* f_ = nullptr;
    std::string path_;
    std::vector<std::string> ring_;
    size_t ring_at_ = 0;
};

extern Log g_log;

// False when stdout is redirected: no colour codes, no dashboard repaints.
bool stdout_is_console();

#define LOGD(...) ::relay::g_log.logf(::relay::Lv::Dbg, __VA_ARGS__)
#define LOGI(...) ::relay::g_log.logf(::relay::Lv::Info, __VA_ARGS__)
#define LOGW(...) ::relay::g_log.logf(::relay::Lv::Warn, __VA_ARGS__)
#define LOGE(...) ::relay::g_log.logf(::relay::Lv::Err, __VA_ARGS__)

}  // namespace relay
