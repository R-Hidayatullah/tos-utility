// XSM (EMotionFX skeletal motion) parser.
//
// A motion is a set of "submotions", one per animated node (bone). Each
// submotion carries up to four keytracks — position, rotation, scale, scale-
// rotation — and each key is a (value, time) pair where TIME is an absolute
// offset in SECONDS from the start of the motion (float mTime).
//
//   struct XSM_Vector3Key    { FileVector3 mValue; float mTime; };   // 16 bytes
//   struct XSM_QuaternionKey { FileQuaternion mValue; float mTime; };// 20 bytes
//   struct XSM_16BitQuaternionKey { File16BitQuaternion mValue; float mTime; }; // 12
//
// USAGE OF TIME (EMotionFX playback model):
//  * Keys within a track are stored sorted ascending by mTime.
//  * The motion's duration = the largest mTime across all keys (mMaxTime for
//    wavelet motions). mMotionFPS is the *export* sample rate and is
//    informational — playback is time-based, not frame-indexed.
//  * To evaluate a track at time t: find the two keys k0,k1 with
//    k0.mTime <= t <= k1.mTime, compute u = (t - k0.mTime)/(k1.mTime-k0.mTime),
//    then LERP positions/scales and NLERP/SLERP rotations by u.
//  * 16-bit rotation keys (v3 submotions) are dequantized by 1/32767 per
//    component before interpolation.
#pragma once

#include "tos/emfx/emfx_format.h"
#include "tos/emfx/chunk_audit.h"
#include "tos/io/byte_reader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tos::emfx {

struct XsmVec3Key { float x = 0, y = 0, z = 0; float time = 0; };
struct XsmQuatKey { float x = 0, y = 0, z = 0, w = 1; float time = 0; }; // decompressed

struct XsmSubMotion {
    std::string name;
    fmt::FileQuaternion poseRot{}, bindPoseRot{}, poseScaleRot{}, bindPoseScaleRot{};
    fmt::FileVector3 posePos{}, poseScale{}, bindPosePos{}, bindPoseScale{};
    float maxError = 0;
    std::vector<XsmVec3Key> posKeys;
    std::vector<XsmQuatKey> rotKeys;
    std::vector<XsmVec3Key> scaleKeys;
    std::vector<XsmQuatKey> scaleRotKeys;
    int version = 0;                 // submotion struct version (2 or 3)
    bool rotCompressed = false;      // rotations were 16-bit in the file

    // Largest key time in this submotion (its local duration in seconds).
    float maxKeyTime() const;
};

struct XsmMotion {
    fmt::FileHeader header{};
    // Info chunk
    int infoVersion = 0;
    uint32_t motionFPS = 0;
    float importanceFactor = 0;
    float maxAcceptableError = 0;
    std::string sourceApp, originalFilename, compilationDate, motionName;

    std::vector<XsmSubMotion> subMotions;

    // Wavelet-compressed motion (if present): timing summary only.
    bool hasWavelet = false;
    float waveletMaxTime = 0;        // motion duration in seconds
    float waveletSecondsPerChunk = 0;
    float waveletSampleSpacing = 0;
    uint32_t waveletNumChunks = 0;

    std::vector<ChunkAudit> audits;

    // Motion duration in seconds: wavelet maxTime, else the largest key time
    // across all uncompressed submotions.
    float duration() const;
    bool allChunksClean() const;
};

XsmMotion parseXsm(const uint8_t* data, size_t size);
XsmMotion parseXsmFile(const std::string& path);

} // namespace tos::emfx
