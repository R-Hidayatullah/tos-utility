#include "tos/emfx/xpm.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace tos::emfx {

namespace {
using namespace tos::emfx::fmt;
using tos::io::ByteReader;

XpmSubMotion readProgressiveSubMotion(ByteReader& r) {
    auto h = r.read<xpm::ProgressiveSubMotion>();
    XpmSubMotion sm;
    sm.poseWeight = h.mPoseWeight;
    sm.minWeight = h.mMinWeight;
    sm.maxWeight = h.mMaxWeight;
    sm.phonemeSet = h.mPhonemeSet;
    sm.name = r.read_string();
    const float range = h.mMaxWeight - h.mMinWeight;
    sm.keys.reserve(h.mNumKeys);
    for (uint32_t i = 0; i < h.mNumKeys; ++i) {
        auto k = r.read<xpm::UnsignedShortKey>();
        float w = h.mMinWeight + (k.mValue / 65535.0f) * range;
        sm.keys.push_back({k.mTime, k.mValue, w});
    }
    return sm;
}

void readSubMotionsChunk(ByteReader& r, XpmMotion& out) {
    auto h = r.read<xpm::SubMotions>();
    out.subMotions.reserve(h.mNumSubMotions);
    for (uint32_t i = 0; i < h.mNumSubMotions; ++i)
        out.subMotions.push_back(readProgressiveSubMotion(r));
}

} // namespace

XpmMotion parseXpm(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    XpmMotion m;
    m.header = r.read<FileHeader>();
    if (!(m.header.mFourcc[0] == 'X' && m.header.mFourcc[1] == 'P' &&
          m.header.mFourcc[2] == 'M' && m.header.mFourcc[3] == ' '))
        throw std::runtime_error("not an XPM file (bad fourcc)");

    while (r.remaining() >= sizeof(FileChunk)) {
        FileChunk chunk = r.read<FileChunk>();
        const size_t start = r.tell();
        const size_t declaredEnd = start + chunk.mSizeInBytes;
        // Same as XSM: ToS may over-count the SubMotions chunk size, so parse
        // structurally and clamp any realignment to the real file end.

        ChunkAudit a;
        a.id = chunk.mChunkID; a.version = chunk.mVersion;
        a.declaredSize = chunk.mSizeInBytes; a.handled = true;

        try {
            switch (chunk.mChunkID) {
                case xpm::CHUNK_INFO: {
                    auto h = r.read<xpm::Info>();
                    m.infoVersion = static_cast<int>(chunk.mVersion);
                    m.motionFPS = h.mMotionFPS;
                    m.sourceApp = r.read_string();
                    m.originalFilename = r.read_string();
                    m.compilationDate = r.read_string();
                    m.motionName = r.read_string();
                    a.label = "Info";
                    break;
                }
                case xpm::CHUNK_SUBMOTIONS:
                    readSubMotionsChunk(r, m); a.label = "SubMotions"; break;
                case xpm::CHUNK_MOTIONEVENTTABLE:
                    a.handled = false; a.label = "MotionEventTable"; break;
                default:
                    a.handled = false; a.label = "?"; break;
            }
        } catch (const std::exception&) {
            a.handled = false;
        }

        a.consumedSize = static_cast<uint32_t>(r.tell() - start);
        a.exact = a.handled && (r.tell() == declaredEnd);
        a.overRead = a.handled && (r.tell() > declaredEnd);
        a.declOverEof = a.handled && !a.exact && (declaredEnd > size) && (r.tell() == size);
        m.audits.push_back(a);
        size_t target = std::min<size_t>(declaredEnd, size);
        if (r.tell() < target) r.seek(target);
    }
    return m;
}

XpmMotion parseXpmFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseXpm(buf.data(), buf.size());
}

float XpmMotion::duration() const {
    float t = 0;
    for (const auto& sm : subMotions) t = std::max(t, sm.maxKeyTime());
    return t;
}

bool XpmMotion::allChunksClean() const {
    for (const auto& a : audits)
        if (a.handled && !a.exact && !a.overRead && !a.declOverEof) return false;
    return true;
}

} // namespace tos::emfx
