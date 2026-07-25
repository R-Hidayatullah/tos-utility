// ipf_tool — list / verify / extract ToS IPF archives and exercise the VFS.
//
//   ipf_tool list    <file.ipf> [maxN]
//   ipf_tool verify  <file.ipf> [maxN]     extract + CRC-check entries
//   ipf_tool extract <file.ipf> <index> <out>
//   ipf_tool vfs     <gameRoot> [findPath] scan data/+patch/, latest-wins stats
#include "tos/ipf/ipf_archive.h"
#include "tos/ipf/ipf_fs.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace tos::ipf;

static int cmdList(const std::string& path, int maxN) {
    IpfArchive a;
    if (!a.open(path)) { std::printf("failed to open %s\n", path.c_str()); return 1; }
    const auto& h = a.header();
    std::printf("== %s ==\n", path.c_str());
    std::printf("  fileCount=%u  newVersion=%u  versionToPatch=%u  tablePtr=0x%X\n",
                h.fileCount, h.newVersion, h.versionToPatch, h.fileTablePointer);
    int n = 0;
    for (const auto& e : a.entries()) {
        if (maxN >= 0 && n >= maxN) break;
        std::printf("  [%5d] %-52s  csize=%-9u usize=%-9u crc=%08X  (%s)\n",
                    n, e.path.c_str(), e.sizeCompressed, e.sizeUncompressed, e.crc32,
                    e.container.c_str());
        ++n;
    }
    std::printf("  (%zu entries total)\n", a.entries().size());
    return 0;
}

static int cmdVerify(const std::string& path, int maxN) {
    IpfArchive a;
    if (!a.open(path)) { std::printf("failed to open %s\n", path.c_str()); return 1; }
    std::printf("== verify %s (%zu entries) ==\n", path.c_str(), a.entries().size());
    int ok = 0, bad = 0, plain = 0, err = 0, n = 0;
    for (const auto& e : a.entries()) {
        if (maxN >= 0 && n >= maxN) break;
        ++n;
        try {
            auto data = IpfArchive::extract(e);
            if (e.storedPlain()) { ++plain; continue; }
            if (IpfArchive::verifyCrc(e, data)) ++ok;
            else { ++bad; if (bad <= 5) std::printf("  CRC MISMATCH: %s\n", e.path.c_str()); }
        } catch (const std::exception& ex) {
            ++err; if (err <= 5) std::printf("  ERROR %s: %s\n", e.path.c_str(), ex.what());
        }
    }
    std::printf("  ok=%d bad=%d plain=%d err=%d (of %d checked)\n", ok, bad, plain, err, n);
    return (bad || err) ? 2 : 0;
}

static int cmdExtract(const std::string& path, int index, const std::string& out) {
    IpfArchive a;
    if (!a.open(path)) { std::printf("failed to open %s\n", path.c_str()); return 1; }
    if (index < 0 || (size_t)index >= a.entries().size()) { std::printf("bad index\n"); return 1; }
    const auto& e = a.entries()[index];
    auto data = IpfArchive::extract(e);
    std::ofstream f(out, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), data.size());
    std::printf("extracted %s -> %s (%zu bytes, crc %s)\n", e.path.c_str(), out.c_str(),
                data.size(), IpfArchive::verifyCrc(e, data) ? "OK" : "MISMATCH");
    return 0;
}

static int cmdVfs(const std::string& root, const std::string& findPath) {
    IpfFileSystem vfs;
    std::printf("scanning %s ...\n", root.c_str());
    vfs.scanGameRoot(root);
    std::printf("archives=%zu  totalEntries=%zu  uniqueFiles=%zu  overridden=%zu\n",
                vfs.archiveCount(), vfs.totalEntryCount(), vfs.uniqueCount(),
                vfs.overriddenCount());
    if (!findPath.empty()) {
        const IpfEntry* e = vfs.find(findPath);
        if (e) {
            std::printf("found %s in %s (version %u, patch=%d) usize=%u\n",
                        findPath.c_str(), e->archivePath.c_str(), e->archiveVersion,
                        (int)e->fromPatch, e->sizeUncompressed);
        } else {
            // Not an exact vpath: fall back to a case-insensitive substring search.
            std::string needle = findPath;
            for (char& c : needle) c = (char)tolower((unsigned char)c);
            auto files = vfs.files();
            int shown = 0;
            std::printf("substring matches for \"%s\":\n", findPath.c_str());
            for (const auto& f : files) {
                std::string v = f.vpath;
                for (char& c : v) c = (char)tolower((unsigned char)c);
                if (v.find(needle) == std::string::npos) continue;
                std::printf("  %s  (v%u%s)\n", f.vpath.c_str(), f.entry->archiveVersion,
                            f.entry->fromPatch ? ",patch" : "");
                if (++shown >= 40) { std::printf("  ... (more)\n"); break; }
            }
            if (shown == 0) { std::printf("  NOT FOUND\n"); return 1; }
        }
    } else {
        auto files = vfs.files();
        std::printf("first 15 unique vpaths:\n");
        for (size_t i = 0; i < files.size() && i < 15; ++i)
            std::printf("  %s  (v%u%s)\n", files[i].vpath.c_str(),
                        files[i].entry->archiveVersion,
                        files[i].entry->fromPatch ? ",patch" : "");
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage:\n"
                    "  ipf_tool list    <file.ipf> [maxN]\n"
                    "  ipf_tool verify  <file.ipf> [maxN]\n"
                    "  ipf_tool extract <file.ipf> <index> <out>\n"
                    "  ipf_tool vfs     <gameRoot> [findPath]\n");
        return 1;
    }
    std::string cmd = argv[1];
    if (cmd == "list")    return cmdList(argv[2], argc > 3 ? std::atoi(argv[3]) : 40);
    if (cmd == "verify")  return cmdVerify(argv[2], argc > 3 ? std::atoi(argv[3]) : -1);
    if (cmd == "extract") return argc > 4 ? cmdExtract(argv[2], std::atoi(argv[3]), argv[4]) : 1;
    if (cmd == "vfs")     return cmdVfs(argv[2], argc > 3 ? argv[3] : "");
    std::printf("unknown command: %s\n", cmd.c_str());
    return 1;
}
