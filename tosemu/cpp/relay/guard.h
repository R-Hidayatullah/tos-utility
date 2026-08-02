// Patches the client's config files, and guarantees they go back.
//
// Pointing the client at the relay means editing files inside the user's game
// install. The whole risk of this tool is leaving those edits behind: a
// client.xml still aimed at 127.0.0.1 after the relay is gone looks exactly
// like a broken game install, and the cause is invisible.
//
// So the order is: back the file up, write a journal entry describing the
// change, flush the journal to disk, and only then patch. If the process dies
// at any point after that -- Ctrl+C, a kill, a bluescreen -- the journal is on
// disk, and the next run finds it, says so loudly, and restores from the
// backups before touching anything.
//
// The journal is the source of truth, not the process. Deleting it is the last
// step of a successful restore, so its presence always means "files are still
// patched".
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace relay {

struct Entry {
    std::string target;        // file we edited, in the game install
    std::string backup;        // untouched copy of the original
    uint64_t    orig_size = 0;
    uint32_t    orig_crc = 0;
    uint32_t    patched_crc = 0;   // what we left on disk, to detect meddling
    std::string when;              // when we patched it
    std::string why;               // one line, for the log and the dashboard
};

class Guard {
public:
    // `work_dir` holds the journal and the backups. Call before patching.
    bool init(const std::string& work_dir);

    // Restores anything an earlier run left patched. Returns the number of
    // files restored, or -1 if a journal exists but could not be honoured.
    int recover_stale();

    // Backs up `target`, journals it, then writes `content`. A target that is
    // missing or unwritable is reported and skipped -- it is never patched
    // without a recoverable backup in place first.
    bool patch(const std::string& target, const std::string& content,
               const std::string& why);

    // Puts every patched file back. Idempotent, safe from a signal handler
    // path, and safe to call when nothing was patched. Returns the number of
    // files restored; entries that could not be restored stay in the journal
    // so the next run retries them.
    int restore_all();

    bool armed() const;                    // files are currently patched
    std::vector<Entry> entries() const;
    const std::string& journal_path() const { return journal_; }

private:
    bool save_journal() const;             // rewrites the whole file, flushed
    bool load_journal(std::vector<Entry>& out) const;
    static bool clear_readonly(const std::string& path);

    mutable std::mutex m_;
    std::string work_;
    std::string backup_dir_;
    std::string journal_;
    std::vector<Entry> entries_;
};

}  // namespace relay
