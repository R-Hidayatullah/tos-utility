#include "guard.h"

#include <windows.h>

#include <cstdio>
#include <sstream>

#include "common.h"
#include "log.h"

namespace relay {

static const char* kJournalMagic = "#tosrelay-journal\tv1";

bool Guard::init(const std::string& work_dir) {
    std::lock_guard<std::mutex> lk(m_);
    work_ = work_dir;
    backup_dir_ = join_path(work_, "backup");
    journal_ = join_path(work_, "patch_journal.tsv");
    return make_dirs(backup_dir_);
}

bool Guard::clear_readonly(const std::string& path) {
    DWORD a = GetFileAttributesA(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (!(a & FILE_ATTRIBUTE_READONLY)) return true;
    return SetFileAttributesA(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY) != 0;
}

bool Guard::save_journal() const {
    std::ostringstream o;
    o << kJournalMagic << "\n";
    o << "#run\tpid=" << GetCurrentProcessId() << "\tstarted="
      << stamp_human(now_us()) << "\tlog=" << g_log.path() << "\n";
    o << "#columns\ttarget\tbackup\torig_size\torig_crc\tpatched_crc\twhen\twhy\n";
    for (const auto& e : entries_) {
        o << "E\t" << e.target << "\t" << e.backup << "\t" << e.orig_size
          << "\t" << e.orig_crc << "\t" << e.patched_crc << "\t" << e.when
          << "\t" << e.why << "\n";
    }
    return write_file_atomic(journal_, o.str());
}

bool Guard::load_journal(std::vector<Entry>& out) const {
    std::string text;
    if (!read_file(journal_, text)) return false;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string tag;
        Entry e;
        std::string osz, ocrc, pcrc;
        std::getline(ls, tag, '\t');
        if (tag != "E") continue;
        std::getline(ls, e.target, '\t');
        std::getline(ls, e.backup, '\t');
        std::getline(ls, osz, '\t');
        std::getline(ls, ocrc, '\t');
        std::getline(ls, pcrc, '\t');
        std::getline(ls, e.when, '\t');
        std::getline(ls, e.why, '\t');
        if (e.target.empty() || e.backup.empty()) continue;
        e.orig_size = osz.empty() ? 0 : std::stoull(osz);
        e.orig_crc = ocrc.empty() ? 0 : uint32_t(std::stoul(ocrc));
        e.patched_crc = pcrc.empty() ? 0 : uint32_t(std::stoul(pcrc));
        out.push_back(e);
    }
    return true;
}

int Guard::recover_stale() {
    std::vector<Entry> stale;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!file_exists(journal_)) return 0;
        if (!load_journal(stale)) {
            LOGE("a patch journal exists at %s but could not be read -- your "
                 "game files may still be patched; restore them from %s by hand",
                 journal_.c_str(), backup_dir_.c_str());
            return -1;
        }
    }

    LOGW("=== STALE JOURNAL ===");
    LOGW("a previous run left %u file(s) patched and did not restore them",
         unsigned(stale.size()));
    LOGW("journal: %s", journal_.c_str());

    // Adopt them and run the ordinary restore path, so recovery and shutdown
    // cannot drift apart.
    {
        std::lock_guard<std::mutex> lk(m_);
        entries_ = stale;
    }
    int n = restore_all();
    if (n == int(stale.size()))
        LOGW("recovered: %d file(s) put back before starting", n);
    return n;
}

bool Guard::patch(const std::string& target, const std::string& content,
                  const std::string& why) {
    std::string orig;
    if (!read_file(target, orig)) {
        LOGE("patch skipped, cannot read %s", target.c_str());
        return false;
    }

    Entry e;
    e.target = target;
    e.orig_size = orig.size();
    e.orig_crc = crc32(orig);
    e.when = stamp_human(now_us());
    e.why = why;
    e.backup = join_path(backup_dir_,
                         base_name(target) + "." + stamp_compact(now_us()) + ".orig");
    e.patched_crc = crc32(content);

    if (orig == content) {
        LOGI("%s already has the values we want -- left alone", target.c_str());
        return true;
    }
    if (!write_file_atomic(e.backup, orig)) {
        LOGE("patch skipped, cannot write backup %s", e.backup.c_str());
        return false;
    }

    // Journal BEFORE the edit. A crash between these two lines leaves an entry
    // whose target still matches its original -- the restore path treats that
    // as already-restored, which is exactly right.
    {
        std::lock_guard<std::mutex> lk(m_);
        entries_.push_back(e);
        if (!save_journal()) {
            entries_.pop_back();
            LOGE("patch skipped, cannot write journal %s", journal_.c_str());
            return false;
        }
    }

    clear_readonly(target);
    if (!write_file_atomic(target, content)) {
        LOGE("PATCH FAILED for %s (need administrator rights for a file under "
             "Program Files?) -- original is intact", target.c_str());
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].target == e.target && entries_[i].backup == e.backup) {
                entries_.erase(entries_.begin() + i);
                break;
            }
        }
        if (entries_.empty()) DeleteFileA(journal_.c_str());
        else save_journal();
        return false;
    }

    LOGI("patched %s (%s); backup %s", target.c_str(), why.c_str(),
         e.backup.c_str());
    return true;
}

int Guard::restore_all() {
    std::vector<Entry> todo;
    {
        std::lock_guard<std::mutex> lk(m_);
        todo = entries_;
    }
    if (todo.empty()) {
        std::lock_guard<std::mutex> lk(m_);
        if (file_exists(journal_)) DeleteFileA(journal_.c_str());
        return 0;
    }

    std::vector<Entry> failed;
    int done = 0;
    for (auto it = todo.rbegin(); it != todo.rend(); ++it) {
        const Entry& e = *it;
        std::string orig;
        if (!read_file(e.backup, orig)) {
            LOGE("RESTORE FAILED for %s -- backup %s is unreadable; that file "
                 "is STILL PATCHED", e.target.c_str(), e.backup.c_str());
            failed.push_back(e);
            continue;
        }

        std::string current;
        bool have_current = read_file(e.target, current);
        if (have_current && crc32(current) == e.orig_crc) {
            LOGI("%s is already the original -- nothing to restore",
                 e.target.c_str());
            ++done;
            continue;
        }
        if (have_current && crc32(current) != e.patched_crc) {
            // Something else wrote to the file while we held it patched -- the
            // client rewrites serverlist_recent.xml, for one. Keep that version
            // beside the backup rather than destroying it silently, then still
            // put the original back, which is what we promised.
            std::string keep = e.backup + ".changed-" + stamp_compact(now_us());
            write_file_atomic(keep, current);
            LOGW("%s changed while patched; its current contents are saved at "
                 "%s and the original is going back", e.target.c_str(),
                 keep.c_str());
        }

        clear_readonly(e.target);
        if (!write_file_atomic(e.target, orig)) {
            LOGE("RESTORE FAILED for %s -- it is STILL PATCHED. Copy %s over it "
                 "by hand, or rerun with --restore once the file is writable.",
                 e.target.c_str(), e.backup.c_str());
            failed.push_back(e);
            continue;
        }
        LOGI("restored %s", e.target.c_str());
        ++done;
    }

    std::lock_guard<std::mutex> lk(m_);
    entries_ = failed;
    if (entries_.empty()) {
        DeleteFileA(journal_.c_str());
        if (done) LOGI("all %d patched file(s) restored; journal cleared", done);
    } else {
        save_journal();
        LOGE("%u file(s) left patched -- the journal at %s is kept so the next "
             "run retries them", unsigned(entries_.size()), journal_.c_str());
    }
    return done;
}

bool Guard::armed() const {
    std::lock_guard<std::mutex> lk(m_);
    return !entries_.empty();
}

std::vector<Entry> Guard::entries() const {
    std::lock_guard<std::mutex> lk(m_);
    return entries_;
}

}  // namespace relay
