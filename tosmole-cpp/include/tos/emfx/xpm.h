// XPM (EMotionFX progressive morph motion) parser.
//
// An XPM animates MORPH-TARGET WEIGHTS over time (facial expressions, phoneme
// lip-sync, hair/cloth morphs). It mirrors the XSM layout but each submotion is
// a single scalar weight track rather than a transform:
//
//   struct XPM_ProgressiveSubMotion { float poseWeight, minWeight, maxWeight;
//                                     uint32 phonemeSet, numKeys; }  // +name +keys
//   struct XPM_UnsignedShortKey     { float mTime; uint16 mValue; } // 8B (+2 pad)
//
// USAGE OF TIME + WEIGHT:
//  * mTime = absolute seconds from t=0 (same model as XSM).
//  * mValue is a 16-bit compressed weight, unpacked with the submotion's range:
//        weight = minWeight + (mValue / 65535) * (maxWeight - minWeight)
//  * To evaluate at time t: bracket the two keys around t and LERP their
//    unpacked weights. That scalar then blends the matching XAC morph target
//    (XAC_PMorphTarget deltas) into the base mesh. phonemeSet != 0 marks a
//    lip-sync phoneme track.
#pragma once

#include "tos/emfx/emfx_format.h"
#include "tos/emfx/chunk_audit.h"
#include "tos/io/byte_reader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tos::emfx {

struct XpmKey {
    float time = 0;       // seconds
    uint16_t rawValue = 0;// packed 16-bit weight
    float weight = 0;     // unpacked: min + raw/65535*(max-min)
};

struct XpmSubMotion {
    std::string name;
    float poseWeight = 0, minWeight = 0, maxWeight = 0;
    uint32_t phonemeSet = 0;
    std::vector<XpmKey> keys;

    float maxKeyTime() const { return keys.empty() ? 0.f : keys.back().time; }
    bool isPhoneme() const { return phonemeSet != 0; }
};

struct XpmMotion {
    fmt::FileHeader header{};
    int infoVersion = 0;
    uint32_t motionFPS = 0;
    std::string sourceApp, originalFilename, compilationDate, motionName;
    std::vector<XpmSubMotion> subMotions;
    std::vector<ChunkAudit> audits;

    float duration() const;
    bool allChunksClean() const;
};

XpmMotion parseXpm(const uint8_t* data, size_t size);
XpmMotion parseXpmFile(const std::string& path);

} // namespace tos::emfx
