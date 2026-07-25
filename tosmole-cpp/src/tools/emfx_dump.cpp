// emfx_dump — parse .xac files and report per-chunk layout validation.
//
// The FileChunk.mSizeInBytes field is our oracle: if structural parsing of a
// chunk consumes exactly that many bytes, our struct layout matches the file.
#include "tos/emfx/xac.h"
#include "tos/emfx/xsm.h"
#include "tos/emfx/xpm.h"
#include "tos/emfx/xsmtime.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>

using namespace tos::emfx;

static const char* fourcc(const fmt::FileHeader& h) {
    static char b[5]; for (int i=0;i<4;++i) b[i]=(char)h.mFourcc[i]; b[4]=0; return b;
}

static int dumpXac(const std::string& path) {
    XacActor a;
    try {
        a = parseXacFile(path);
    } catch (const std::exception& e) {
        std::printf("  ERROR: %s\n", e.what());
        return 1;
    }
    std::printf("== %s ==\n", path.c_str());
    std::printf("  header: '%s' v%u.%u endian=%u mulOrder=%u\n",
                fourcc(a.header), a.header.mHiVersion, a.header.mLoVersion,
                a.header.mEndianType, a.header.mMulOrder);
    std::printf("  nodes=%zu meshes=%zu stdMat=%zu fxMat=%zu skinning=%zu"
                " nodeGroups=%zu morphTargets=%zu meshLODs=%zu\n",
                a.nodes.size(), a.meshes.size(), a.stdMaterials.size(),
                a.fxMaterials.size(), a.skinnings.size(),
                a.nodeGroups.size(), a.morphTargets.size(), a.meshLodLevels.size());

    int bad = 0;
    std::printf("  chunks: %zu\n", a.audits.size());
    for (const auto& c : a.audits) {
        const char* status = !c.handled ? "SKIP"
                           : c.exact    ? "OK  "
                           : c.overRead ? "OVER"   // structural read past under-counted size
                                        : "MISMATCH";
        if (c.handled && !c.exact && !c.overRead) ++bad;
        std::printf("    [%s] id=%2u v=%u size=%-7u consumed=%-7u %s\n",
                    status, c.id, c.version, c.declaredSize, c.consumedSize,
                    c.label.c_str());
    }
    auto texs = a.textureNames();
    if (!texs.empty()) {
        std::printf("  textures (%zu):\n", texs.size());
        for (const auto& t : texs) std::printf("    - %s\n", t.c_str());
    }
    if (!a.nodes.empty()) {
        std::printf("  first %zu node names:\n", a.nodes.size() < 8 ? a.nodes.size() : 8);
        size_t n = a.nodes.size() < 8 ? a.nodes.size() : 8;
        for (size_t i = 0; i < n; ++i)
            std::printf("    - [%zu] %s (parent=%d)\n", i, a.nodes[i].name.c_str(),
                        (int)a.nodes[i].parentIndex);
    }
    std::printf("  => %s\n\n", bad == 0 ? "ALL HANDLED CHUNKS EXACT" : "LAYOUT MISMATCH DETECTED");
    return bad == 0 ? 0 : 2;
}

static int dumpXsm(const std::string& path) {
    XsmMotion m;
    try {
        m = parseXsmFile(path);
    } catch (const std::exception& e) {
        std::printf("  ERROR: %s\n", e.what());
        return 1;
    }
    std::printf("== %s ==\n", path.c_str());
    std::printf("  header: '%s' v%u.%u endian=%u mulOrder=%u\n",
                fourcc(m.header), m.header.mHiVersion, m.header.mLoVersion,
                m.header.mEndianType, m.header.mMulOrder);
    std::printf("  info v%d: fps=%u  name='%s' app='%s'\n",
                m.infoVersion, m.motionFPS, m.motionName.c_str(), m.sourceApp.c_str());
    std::printf("  submotions=%zu wavelet=%s  DURATION=%.4f s\n",
                m.subMotions.size(), m.hasWavelet ? "yes" : "no", m.duration());
    if (m.hasWavelet)
        std::printf("  wavelet: maxTime=%.4f secondsPerChunk=%.4f sampleSpacing=%.5f chunks=%u\n",
                    m.waveletMaxTime, m.waveletSecondsPerChunk, m.waveletSampleSpacing,
                    m.waveletNumChunks);

    int bad = 0;
    std::printf("  chunks: %zu\n", m.audits.size());
    for (const auto& c : m.audits) {
        const char* status = !c.handled   ? "SKIP"
                           : c.exact       ? "OK  "
                           : c.overRead    ? "OVER"
                           : c.declOverEof ? "OVER-DECL" : "MISMATCH";
        if (c.handled && !c.exact && !c.overRead && !c.declOverEof) ++bad;
        std::printf("    [%s] id=%3u v=%u size=%-7u consumed=%-7u %s\n",
                    status, c.id, c.version, c.declaredSize, c.consumedSize, c.label.c_str());
    }
    // Show the timing of the first few submotions: key counts + local time range.
    size_t n = std::min<size_t>(m.subMotions.size(), 6);
    if (n) std::printf("  first %zu submotions (key counts + time range):\n", n);
    for (size_t i = 0; i < n; ++i) {
        const auto& sm = m.subMotions[i];
        float firstRot = sm.rotKeys.empty() ? 0 : sm.rotKeys.front().time;
        float lastRot  = sm.rotKeys.empty() ? 0 : sm.rotKeys.back().time;
        std::printf("    - %-24s pos=%-4zu rot=%-4zu scale=%-4zu scaleRot=%-4zu"
                    " rotTime=[%.3f..%.3f] cmp=%d\n",
                    sm.name.c_str(), sm.posKeys.size(), sm.rotKeys.size(),
                    sm.scaleKeys.size(), sm.scaleRotKeys.size(), firstRot, lastRot,
                    (int)sm.rotCompressed);
    }
    // Concrete look at TIME: pick the submotion with the most rotation keys and
    // print its first/last key times — these are absolute seconds from t=0.
    const XsmSubMotion* rich = nullptr;
    for (const auto& sm : m.subMotions)
        if (!rich || sm.rotKeys.size() > rich->rotKeys.size()) rich = &sm;
    if (rich && !rich->rotKeys.empty()) {
        std::printf("  richest track '%s': %zu rot keys, time %.4f..%.4f s\n",
                    rich->name.c_str(), rich->rotKeys.size(),
                    rich->rotKeys.front().time, rich->rotKeys.back().time);
        size_t k = std::min<size_t>(rich->rotKeys.size(), 5);
        for (size_t i = 0; i < k; ++i) {
            const auto& q = rich->rotKeys[i];
            std::printf("      key[%zu] t=%.4f s  quat=(% .4f,% .4f,% .4f,% .4f)\n",
                        i, q.time, q.x, q.y, q.z, q.w);
        }
        if (m.motionFPS)
            std::printf("      (fps=%u => frame spacing ~%.4f s; last key ~frame %.1f)\n",
                        m.motionFPS, 1.0f / m.motionFPS, rich->rotKeys.back().time * m.motionFPS);
    }
    std::printf("  => %s\n\n", bad == 0 ? "ALL HANDLED CHUNKS CLEAN" : "LAYOUT MISMATCH DETECTED");
    return bad == 0 ? 0 : 2;
}

static int dumpXpm(const std::string& path) {
    XpmMotion m;
    try {
        m = parseXpmFile(path);
    } catch (const std::exception& e) {
        std::printf("  ERROR: %s\n", e.what());
        return 1;
    }
    std::printf("== %s ==\n", path.c_str());
    std::printf("  header: '%s' v%u.%u endian=%u mulOrder=%u\n",
                fourcc(m.header), m.header.mHiVersion, m.header.mLoVersion,
                m.header.mEndianType, m.header.mMulOrder);
    std::printf("  info v%d: fps=%u name='%s' app='%s'\n",
                m.infoVersion, m.motionFPS, m.motionName.c_str(), m.sourceApp.c_str());
    size_t phon = 0;
    for (const auto& sm : m.subMotions) if (sm.isPhoneme()) ++phon;
    std::printf("  submotions=%zu (phoneme=%zu)  DURATION=%.4f s\n",
                m.subMotions.size(), phon, m.duration());

    int bad = 0;
    std::printf("  chunks: %zu\n", m.audits.size());
    for (const auto& c : m.audits) {
        const char* status = !c.handled   ? "SKIP"
                           : c.exact       ? "OK  "
                           : c.overRead    ? "OVER"
                           : c.declOverEof ? "OVER-DECL" : "MISMATCH";
        if (c.handled && !c.exact && !c.overRead && !c.declOverEof) ++bad;
        std::printf("    [%s] id=%3u v=%u size=%-7u consumed=%-7u %s\n",
                    status, c.id, c.version, c.declaredSize, c.consumedSize, c.label.c_str());
    }
    // Show the richest weight track: min/max range + first key times & weights.
    const XpmSubMotion* rich = nullptr;
    for (const auto& sm : m.subMotions)
        if (!rich || sm.keys.size() > rich->keys.size()) rich = &sm;
    if (rich && !rich->keys.empty()) {
        std::printf("  richest morph '%s': %zu keys, weight range [%.3f..%.3f], phoneme=%u\n",
                    rich->name.c_str(), rich->keys.size(), rich->minWeight, rich->maxWeight,
                    rich->phonemeSet);
        size_t k = std::min<size_t>(rich->keys.size(), 5);
        for (size_t i = 0; i < k; ++i)
            std::printf("      key[%zu] t=%.4f s  raw=%-5u weight=%.4f\n",
                        i, rich->keys[i].time, rich->keys[i].rawValue, rich->keys[i].weight);
    }
    std::printf("  => %s\n\n", bad == 0 ? "ALL HANDLED CHUNKS CLEAN" : "LAYOUT MISMATCH DETECTED");
    return bad == 0 ? 0 : 2;
}

static int dumpXsmTime(const std::string& path) {
    XsmTime t;
    try {
        t = parseXsmTimeFile(path);
    } catch (const std::exception& e) {
        std::printf("  ERROR: %s\n", e.what());
        return 1;
    }
    std::printf("== %s ==\n", path.c_str());
    std::printf("  marker=0x%08X  keys=%zu  duration=%.4f s  sizeExact=%s\n",
                (unsigned)t.marker, t.keys.size(), t.duration(),
                t.sizeExact ? "yes" : "NO");
    size_t nonzero = 0;
    for (const auto& k : t.keys) if (k.value != 0) ++nonzero;
    std::printf("  entries with nonzero value: %zu\n", nonzero);
    size_t n = t.keys.size() < 12 ? t.keys.size() : 12;
    for (size_t i = 0; i < n; ++i)
        std::printf("    key[%2zu] t=%.4f s  value=%u\n", i, t.keys[i].time, t.keys[i].value);
    std::printf("  => %s\n\n", t.sizeExact ? "SIZE EXACT" : "SIZE MISMATCH");
    return t.sizeExact ? 0 : 2;
}

static std::string extLower(const std::string& p) {
    auto dot = p.find_last_of('.');
    std::string e = (dot == std::string::npos) ? "" : p.substr(dot);
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c){ return std::tolower(c); });
    return e;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: emfx_dump <file.xac|.xsm> [more ...]\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; ++i) {
        std::string p = argv[i];
        std::string e = extLower(p);
        if (e == ".xsm")          rc |= dumpXsm(p);
        else if (e == ".xpm")     rc |= dumpXpm(p);
        else if (e == ".xsmtime") rc |= dumpXsmTime(p);
        else                      rc |= dumpXac(p);
    }
    return rc;
}
