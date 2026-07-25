#include "tos/emfx/xsm.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace tos::emfx {

namespace {
using namespace tos::emfx::fmt;
using tos::io::ByteReader;

constexpr float kQuat16Scale = 1.0f / 32767.0f; // MCore 16-bit quaternion dequant

XsmVec3Key readVec3Key(ByteReader& r) {
    auto k = r.read<xsm::Vector3Key>();
    return {k.mValue.mX, k.mValue.mY, k.mValue.mZ, k.mTime};
}
XsmQuatKey readQuatKey(ByteReader& r) {
    auto k = r.read<xsm::QuaternionKey>();
    return {k.mValue.mX, k.mValue.mY, k.mValue.mZ, k.mValue.mW, k.mTime};
}
XsmQuatKey readQuat16Key(ByteReader& r) {
    auto k = r.read<xsm::Quaternion16Key>();
    return {k.mValue.mX * kQuat16Scale, k.mValue.mY * kQuat16Scale,
            k.mValue.mZ * kQuat16Scale, k.mValue.mW * kQuat16Scale, k.mTime};
}

// One submotion, container version 1 => SkeletalSubMotion2 (float quats),
// container version 2 => SkeletalSubMotion3 (16-bit quats).
XsmSubMotion readSubMotion(ByteReader& r, bool compressed) {
    XsmSubMotion sm;
    sm.rotCompressed = compressed;
    uint32_t numPos, numRot, numScale, numScaleRot;
    if (!compressed) {
        auto h = r.read<xsm::SkeletalSubMotion2>();
        sm.version = 2;
        sm.poseRot = h.mPoseRot; sm.bindPoseRot = h.mBindPoseRot;
        sm.poseScaleRot = h.mPoseScaleRot; sm.bindPoseScaleRot = h.mBindPoseScaleRot;
        sm.posePos = h.mPosePos; sm.poseScale = h.mPoseScale;
        sm.bindPosePos = h.mBindPosePos; sm.bindPoseScale = h.mBindPoseScale;
        sm.maxError = h.mMaxError;
        numPos = h.mNumPosKeys; numRot = h.mNumRotKeys;
        numScale = h.mNumScaleKeys; numScaleRot = h.mNumScaleRotKeys;
    } else {
        auto h = r.read<xsm::SkeletalSubMotion3>();
        sm.version = 3;
        auto dq = [](const File16BitQuaternion& q) {
            return FileQuaternion{q.mX * kQuat16Scale, q.mY * kQuat16Scale,
                                  q.mZ * kQuat16Scale, q.mW * kQuat16Scale};
        };
        sm.poseRot = dq(h.mPoseRot); sm.bindPoseRot = dq(h.mBindPoseRot);
        sm.poseScaleRot = dq(h.mPoseScaleRot); sm.bindPoseScaleRot = dq(h.mBindPoseScaleRot);
        sm.posePos = h.mPosePos; sm.poseScale = h.mPoseScale;
        sm.bindPosePos = h.mBindPosePos; sm.bindPoseScale = h.mBindPoseScale;
        sm.maxError = h.mMaxError;
        numPos = h.mNumPosKeys; numRot = h.mNumRotKeys;
        numScale = h.mNumScaleKeys; numScaleRot = h.mNumScaleRotKeys;
    }
    sm.name = r.read_string();

    sm.posKeys.reserve(numPos);
    for (uint32_t i = 0; i < numPos; ++i) sm.posKeys.push_back(readVec3Key(r));
    sm.rotKeys.reserve(numRot);
    for (uint32_t i = 0; i < numRot; ++i)
        sm.rotKeys.push_back(compressed ? readQuat16Key(r) : readQuatKey(r));
    sm.scaleKeys.reserve(numScale);
    for (uint32_t i = 0; i < numScale; ++i) sm.scaleKeys.push_back(readVec3Key(r));
    sm.scaleRotKeys.reserve(numScaleRot);
    for (uint32_t i = 0; i < numScaleRot; ++i)
        sm.scaleRotKeys.push_back(compressed ? readQuat16Key(r) : readQuatKey(r));
    return sm;
}

void readSubMotionsChunk(ByteReader& r, uint32_t version, XsmMotion& out) {
    auto h = r.read<xsm::SubMotions>();
    const bool compressed = (version >= 2); // v2 container = 16-bit rotation keys
    out.subMotions.reserve(h.mNumSubMotions);
    for (uint32_t i = 0; i < h.mNumSubMotions; ++i)
        out.subMotions.push_back(readSubMotion(r, compressed));
}

void readWaveletInfoChunk(ByteReader& r, XsmMotion& out) {
    auto h = r.read<xsm::WaveletInfo>();
    out.hasWavelet = true;
    out.waveletMaxTime = h.mMaxTime;
    out.waveletSecondsPerChunk = h.mSecondsPerChunk;
    out.waveletSampleSpacing = h.mSampleSpacing;
    out.waveletNumChunks = h.mNumChunks;
    // Skip mappings + submotions + compressed chunks (timing-only for now).
    for (uint32_t i = 0; i < h.mNumSubMotions; ++i) r.read<xsm::WaveletMapping>();
    for (uint32_t i = 0; i < h.mNumSubMotions; ++i) {
        r.read<xsm::WaveletSkeletalSubMotion>();
        r.read_string(); // motion part name
    }
    for (uint32_t c = 0; c < h.mNumChunks; ++c) {
        auto wc = r.read<xsm::WaveletChunk>();
        r.skip(wc.mCompressedRotNumBytes);
        r.skip(wc.mCompressedPosNumBytes);
        r.skip(wc.mCompressedScaleNumBytes);
    }
}

} // namespace

XsmMotion parseXsm(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    XsmMotion m;
    m.header = r.read<FileHeader>();
    if (!(m.header.mFourcc[0] == 'X' && m.header.mFourcc[1] == 'S' &&
          m.header.mFourcc[2] == 'M' && m.header.mFourcc[3] == ' '))
        throw std::runtime_error("not an XSM file (bad fourcc)");

    while (r.remaining() >= sizeof(FileChunk)) {
        FileChunk chunk = r.read<FileChunk>();
        const size_t start = r.tell();
        const size_t declaredEnd = start + chunk.mSizeInBytes;
        // NOTE: ToS .xsm SubMotions chunks over-count mSizeInBytes (the declared
        // size runs past EOF), so we do NOT bail on declaredEnd > size. Instead
        // we parse structurally and clamp any realignment to the real file end.

        ChunkAudit a;
        a.id = chunk.mChunkID; a.version = chunk.mVersion;
        a.declaredSize = chunk.mSizeInBytes; a.handled = true;

        try {
            switch (chunk.mChunkID) {
                case xsm::CHUNK_INFO:
                    switch (chunk.mVersion) {
                        case 1: { auto h = r.read<xsm::Info>();  m.motionFPS = h.mMotionFPS; break; }
                        case 2: { auto h = r.read<xsm::Info2>(); m.motionFPS = h.mMotionFPS;
                                  m.importanceFactor = h.mImportanceFactor;
                                  m.maxAcceptableError = h.mMaxAcceptableError; break; }
                        case 3: { auto h = r.read<xsm::Info3>(); m.motionFPS = h.mMotionFPS;
                                  m.importanceFactor = h.mImportanceFactor;
                                  m.maxAcceptableError = h.mMaxAcceptableError; break; }
                        default: a.handled = false; break;
                    }
                    if (a.handled) {
                        m.infoVersion = static_cast<int>(chunk.mVersion);
                        m.sourceApp = r.read_string();
                        m.originalFilename = r.read_string();
                        m.compilationDate = r.read_string();
                        m.motionName = r.read_string();
                    }
                    a.label = "Info";
                    break;
                case xsm::CHUNK_SUBMOTIONS:
                    readSubMotionsChunk(r, chunk.mVersion, m); a.label = "SubMotions"; break;
                case xsm::CHUNK_WAVELETINFO:
                    readWaveletInfoChunk(r, m); a.label = "WaveletInfo"; break;
                case xsm::CHUNK_MOTIONEVENTTABLE:
                    a.handled = false; a.label = "MotionEventTable"; break; // skipped
                default:
                    a.handled = false; a.label = "?"; break;
            }
        } catch (const std::exception&) {
            a.handled = false;
        }

        a.consumedSize = static_cast<uint32_t>(r.tell() - start);
        a.exact = a.handled && (r.tell() == declaredEnd);
        a.overRead = a.handled && (r.tell() > declaredEnd);
        // Accepted quirk: declared size runs past EOF but the structural parse
        // consumed everything cleanly to the real file end (ToS over-counts the
        // SubMotions chunk size; EMotionFX reads submotions by count anyway).
        a.declOverEof = a.handled && !a.exact && (declaredEnd > size) && (r.tell() == size);
        m.audits.push_back(a);
        size_t target = std::min<size_t>(declaredEnd, size);
        if (r.tell() < target) r.seek(target);
    }
    return m;
}

XsmMotion parseXsmFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseXsm(buf.data(), buf.size());
}

float XsmSubMotion::maxKeyTime() const {
    float t = 0;
    if (!posKeys.empty())      t = std::max(t, posKeys.back().time);
    if (!rotKeys.empty())      t = std::max(t, rotKeys.back().time);
    if (!scaleKeys.empty())    t = std::max(t, scaleKeys.back().time);
    if (!scaleRotKeys.empty()) t = std::max(t, scaleRotKeys.back().time);
    return t;
}

float XsmMotion::duration() const {
    if (hasWavelet) return waveletMaxTime;
    float t = 0;
    for (const auto& sm : subMotions) t = std::max(t, sm.maxKeyTime());
    return t;
}

bool XsmMotion::allChunksClean() const {
    for (const auto& a : audits)
        if (a.handled && !a.exact && !a.overRead && !a.declOverEof) return false;
    return true;
}

} // namespace tos::emfx
