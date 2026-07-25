// EMotionFX on-disk file-format structures for XAC / XSM / XPM.
//
// These mirror the authoritative EMotionFX 3.9 SDK headers
// (Source/Importer/{SharedFileFormatStructs,XACFileFormat,XSMFileFormat,
//  XPMFileFormat}.h) byte-for-byte, using DEFAULT struct alignment — exactly
// as the exporter serialized them with `Write(&s, sizeof(s))`.
//
// The static_assert on each struct's size documents and locks the on-disk
// layout (padding included). Max field alignment in every struct here is 4
// bytes, so MinGW/GCC and the original MSVC produce identical layouts.
#pragma once

#include <cstdint>
#include <type_traits>

namespace tos::emfx::fmt {

using uint8  = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using int8   = int8_t;
using int16  = int16_t;
using int32  = int32_t;

// ---------------------------------------------------------------------------
// Shared chunk IDs / enums
// ---------------------------------------------------------------------------
enum : uint32 {
    SHARED_CHUNK_MOTIONEVENTTABLE = 50,
    SHARED_CHUNK_TIMESTAMP        = 51,
};

enum : uint8 {
    MULORDER_SCALE_ROT_TRANS = 0,
    MULORDER_ROT_SCALE_TRANS = 1,
};

// ---------------------------------------------------------------------------
// Shared primitive structs
// ---------------------------------------------------------------------------
struct FileChunk { uint32 mChunkID; uint32 mSizeInBytes; uint32 mVersion; };
struct FileColor { float mR, mG, mB, mA; };
struct FileVector3 { float mX, mY, mZ; };
struct File16BitVector3 { uint16 mX, mY, mZ; };
struct File8BitVector3 { uint8 mX, mY, mZ; };
struct FileQuaternion { float mX, mY, mZ, mW; };
struct File16BitQuaternion { int16 mX, mY, mZ, mW; };

static_assert(sizeof(FileChunk) == 12);
static_assert(sizeof(FileColor) == 16);
static_assert(sizeof(FileVector3) == 12);
static_assert(sizeof(File16BitVector3) == 6);
static_assert(sizeof(File8BitVector3) == 3);
static_assert(sizeof(FileQuaternion) == 16);
static_assert(sizeof(File16BitQuaternion) == 8);

// File header shared shape (fourcc differs per format)
struct FileHeader {
    uint8 mFourcc[4];   // "XAC ", "XSM ", "XPM "
    uint8 mHiVersion;
    uint8 mLoVersion;
    uint8 mEndianType;  // 0 = little, 1 = big
    uint8 mMulOrder;
};
static_assert(sizeof(FileHeader) == 8);

// ===========================================================================
// XAC — actor (skeleton + mesh + materials)
// ===========================================================================
namespace xac {

enum : uint32 {
    CHUNK_NODE              = 0,
    CHUNK_MESH              = 1,
    CHUNK_SKINNINGINFO      = 2,
    CHUNK_STDMATERIAL       = 3,
    CHUNK_STDMATERIALLAYER  = 4,
    CHUNK_FXMATERIAL        = 5,
    CHUNK_LIMIT             = 6,
    CHUNK_INFO              = 7,
    CHUNK_MESHLODLEVELS     = 8,
    CHUNK_STDPROGMORPHTARGET= 9,
    CHUNK_NODEGROUPS        = 10,
    CHUNK_NODES             = 11,
    CHUNK_STDPMORPHTARGETS  = 12,
    CHUNK_MATERIALINFO      = 13,
    CHUNK_NODEMOTIONSOURCES = 14,
    CHUNK_ATTACHMENTNODES   = 15,
};

struct Info {
    uint32 mRepositioningMask;
    uint32 mRepositioningNodeIndex;
    uint8  mExporterHighVersion;
    uint8  mExporterLowVersion;
    // + 4 strings
};
struct Info2 {
    uint32 mRepositioningMask;
    uint32 mRepositioningNodeIndex;
    uint8  mExporterHighVersion;
    uint8  mExporterLowVersion;
    float  mRetargetRootOffset;   // compiler pads 2 bytes BEFORE this float
    // + 4 strings
};
struct Info3 {
    uint32 mTrajectoryNodeIndex;
    uint32 mMotionExtractionNodeIndex;
    uint32 mMotionExtractionMask;
    uint8  mExporterHighVersion;
    uint8  mExporterLowVersion;
    float  mRetargetRootOffset;
};
struct Info4 {
    uint32 mNumLODs;
    uint32 mTrajectoryNodeIndex;
    uint32 mMotionExtractionNodeIndex;
    uint8  mExporterHighVersion;
    uint8  mExporterLowVersion;
    float  mRetargetRootOffset;
};
static_assert(sizeof(Info)  == 12);
static_assert(sizeof(Info2) == 16);
static_assert(sizeof(Info3) == 20);
static_assert(sizeof(Info4) == 20);

struct Node {
    FileQuaternion mLocalQuat;
    FileQuaternion mScaleRot;
    FileVector3 mLocalPos;
    FileVector3 mLocalScale;
    FileVector3 mShear;
    uint32 mSkeletalLODs;
    uint32 mParentIndex;
};
struct Node2 {
    FileQuaternion mLocalQuat, mScaleRot;
    FileVector3 mLocalPos, mLocalScale, mShear;
    uint32 mSkeletalLODs;
    uint32 mParentIndex;
    uint8  mNodeFlags;   // + 3 pad
};
struct Node3 {
    FileQuaternion mLocalQuat, mScaleRot;
    FileVector3 mLocalPos, mLocalScale, mShear;
    uint32 mSkeletalLODs;
    uint32 mParentIndex;
    uint8  mNodeFlags;   // 3 pad BEFORE mOBB
    float  mOBB[16];
};
struct Node4 {
    FileQuaternion mLocalQuat, mScaleRot;
    FileVector3 mLocalPos, mLocalScale, mShear;
    uint32 mSkeletalLODs;
    uint32 mMotionLODs;
    uint32 mParentIndex;
    uint32 mNumChilds;
    uint8  mNodeFlags;   // 3 pad BEFORE mOBB
    float  mOBB[16];
    float  mImportanceFactor;
};
static_assert(sizeof(Node)  == 76);
static_assert(sizeof(Node2) == 80);
static_assert(sizeof(Node3) == 144);
static_assert(sizeof(Node4) == 156);

struct Nodes { uint32 mNumNodes; uint32 mNumRootNodes; /* + Node4[mNumNodes] */ };
static_assert(sizeof(Nodes) == 8);

struct MeshLODLevel { uint32 mLODLevel; uint32 mSizeInBytes; /* + bytes */ };

struct SkinInfluence { float mWeight; uint16 mNodeNr; /* +2 pad */ };
static_assert(sizeof(SkinInfluence) == 8);

struct SkinningInfo  { uint32 mNodeIndex; uint8 mIsForCollisionMesh; };
struct SkinningInfo2 { uint32 mNodeIndex; uint32 mNumTotalInfluences; uint8 mIsForCollisionMesh; };
struct SkinningInfo3 { uint32 mNodeIndex; uint32 mNumLocalBones; uint32 mNumTotalInfluences; uint8 mIsForCollisionMesh; };
struct SkinningInfo4 { uint32 mNodeIndex; uint32 mLOD; uint32 mNumLocalBones; uint32 mNumTotalInfluences; uint8 mIsForCollisionMesh; };
struct SkinningInfoTableEntry { uint32 mStartIndex; uint32 mNumElements; };
static_assert(sizeof(SkinningInfo)  == 8);
static_assert(sizeof(SkinningInfo2) == 12);
static_assert(sizeof(SkinningInfo3) == 16);
static_assert(sizeof(SkinningInfo4) == 20);
static_assert(sizeof(SkinningInfoTableEntry) == 8);

struct StandardMaterial {
    FileColor mAmbient, mDiffuse, mSpecular, mEmissive;
    float mShine, mShineStrength, mOpacity, mIOR;
    uint8 mDoubleSided, mWireFrame, mTransparencyType;
};
struct StandardMaterial2 {
    FileColor mAmbient, mDiffuse, mSpecular, mEmissive;
    float mShine, mShineStrength, mOpacity, mIOR;
    uint8 mDoubleSided, mWireFrame, mTransparencyType, mNumLayers;
};
struct StandardMaterial3 {
    uint32 mLOD;
    FileColor mAmbient, mDiffuse, mSpecular, mEmissive;
    float mShine, mShineStrength, mOpacity, mIOR;
    uint8 mDoubleSided, mWireFrame, mTransparencyType, mNumLayers;
};
static_assert(sizeof(StandardMaterial)  == 84);
static_assert(sizeof(StandardMaterial2) == 84);
static_assert(sizeof(StandardMaterial3) == 88);

struct StandardMaterialLayer {
    float mAmount, mUOffset, mVOffset, mUTiling, mVTiling, mRotationRadians;
    uint16 mMaterialNumber;
    uint8  mMapType;  // +1 pad
};
struct StandardMaterialLayer2 {
    float mAmount, mUOffset, mVOffset, mUTiling, mVTiling, mRotationRadians;
    uint16 mMaterialNumber;
    uint8  mMapType;
    uint8  mBlendMode;
};
static_assert(sizeof(StandardMaterialLayer)  == 28);
static_assert(sizeof(StandardMaterialLayer2) == 28);

struct VertexAttributeLayer {
    uint32 mLayerTypeID;
    uint32 mAttribSizeInBytes;
    uint8  mEnableDeformations;
    uint8  mIsScale;   // +2 pad
    // + mAttribSizeInBytes * mesh.mTotalVerts bytes
};
static_assert(sizeof(VertexAttributeLayer) == 12);

struct SubMesh {
    uint32 mNumIndices, mNumVerts, mMaterialIndex, mNumBones;
    // + uint32[mNumIndices] + uint32[mNumBones]
};
static_assert(sizeof(SubMesh) == 16);

struct Mesh {
    uint32 mNodeIndex, mNumOrgVerts, mTotalVerts, mTotalIndices, mNumSubMeshes, mNumLayers;
    uint8  mIsCollisionMesh;  // +3 pad
};
struct Mesh2 {
    uint32 mNodeIndex, mLOD, mNumOrgVerts, mTotalVerts, mTotalIndices, mNumSubMeshes, mNumLayers;
    uint8  mIsCollisionMesh;  // +3 pad
};
static_assert(sizeof(Mesh)  == 28);
static_assert(sizeof(Mesh2) == 32);

struct Limit {
    FileVector3 mTranslationMin, mTranslationMax, mRotationMin, mRotationMax, mScaleMin, mScaleMax;
    uint8  mLimitFlags[9];   // 3 pad BEFORE mNodeNumber
    uint32 mNodeNumber;
};
static_assert(sizeof(Limit) == 88);

struct MaterialInfo  { uint32 mNumTotalMaterials, mNumStandardMaterials, mNumFXMaterials; };
struct MaterialInfo2 { uint32 mLOD, mNumTotalMaterials, mNumStandardMaterials, mNumFXMaterials; };
static_assert(sizeof(MaterialInfo)  == 12);
static_assert(sizeof(MaterialInfo2) == 16);

struct PMorphTarget {
    float mRangeMin, mRangeMax;
    uint32 mLOD, mNumMeshDeformDeltas, mNumTransformations, mPhonemeSets;
    // + string + deltas + transforms
};
struct PMorphTargets { uint32 mNumMorphTargets; uint32 mLOD; };

struct NodeGroup { uint16 mNumNodes; uint8 mDisabledOnDefault; }; // +1 pad
struct PMorphTargetMeshDeltas { uint32 mNodeIndex; float mMinValue, mMaxValue; uint32 mNumVertices; };
struct PMorphTargetTransform {
    uint32 mNodeIndex;
    FileQuaternion mRotation, mScaleRotation;
    FileVector3 mPosition, mScale;
};
static_assert(sizeof(PMorphTarget) == 24);
static_assert(sizeof(PMorphTargets) == 8);
static_assert(sizeof(PMorphTargetMeshDeltas) == 16);
static_assert(sizeof(PMorphTargetTransform) == 60);
static_assert(sizeof(NodeGroup) == 4);

} // namespace xac

// ===========================================================================
// XSM — skeletal motion
// ===========================================================================
namespace xsm {

enum : uint32 {
    CHUNK_SUBMOTION       = 200,
    CHUNK_INFO            = 201,
    CHUNK_MOTIONEVENTTABLE= SHARED_CHUNK_MOTIONEVENTTABLE,
    CHUNK_SUBMOTIONS      = 202,
    CHUNK_WAVELETINFO     = 203,
};

struct Info  { uint32 mMotionFPS; uint8 mExporterHighVersion, mExporterLowVersion; };
struct Info2 { float mImportanceFactor, mMaxAcceptableError; uint32 mMotionFPS; uint8 mExporterHighVersion, mExporterLowVersion; };
struct Info3 { float mImportanceFactor, mMaxAcceptableError; uint32 mMotionFPS, mMotionExtractionMask; uint8 mExporterHighVersion, mExporterLowVersion; };
static_assert(sizeof(Info)  == 8);
static_assert(sizeof(Info2) == 16);
static_assert(sizeof(Info3) == 20);

struct Vector3Key    { FileVector3 mValue; float mTime; };
struct QuaternionKey { FileQuaternion mValue; float mTime; };
struct Quaternion16Key { File16BitQuaternion mValue; float mTime; };
static_assert(sizeof(Vector3Key) == 16);
static_assert(sizeof(QuaternionKey) == 20);
static_assert(sizeof(Quaternion16Key) == 12);

struct SkeletalSubMotion {
    FileQuaternion mPoseRot, mBindPoseRot, mPoseScaleRot, mBindPoseScaleRot;
    FileVector3 mPosePos, mPoseScale, mBindPosePos, mBindPoseScale;
    uint32 mNumPosKeys, mNumRotKeys, mNumScaleKeys, mNumScaleRotKeys;
};
struct SkeletalSubMotion2 {
    FileQuaternion mPoseRot, mBindPoseRot, mPoseScaleRot, mBindPoseScaleRot;
    FileVector3 mPosePos, mPoseScale, mBindPosePos, mBindPoseScale;
    uint32 mNumPosKeys, mNumRotKeys, mNumScaleKeys, mNumScaleRotKeys;
    float mMaxError;
};
struct SkeletalSubMotion3 {
    File16BitQuaternion mPoseRot, mBindPoseRot, mPoseScaleRot, mBindPoseScaleRot;
    FileVector3 mPosePos, mPoseScale, mBindPosePos, mBindPoseScale;
    uint32 mNumPosKeys, mNumRotKeys, mNumScaleKeys, mNumScaleRotKeys;
    float mMaxError;
};
static_assert(sizeof(SkeletalSubMotion)  == 128);
static_assert(sizeof(SkeletalSubMotion2) == 132);
static_assert(sizeof(SkeletalSubMotion3) == 100);

struct SubMotions  { uint32 mNumSubMotions; };
struct WaveletMapping { uint16 mPosIndex, mRotIndex, mScaleRotIndex, mScaleIndex; };
static_assert(sizeof(WaveletMapping) == 8);

struct WaveletInfo {
    uint32 mNumChunks, mSamplesPerChunk;
    uint32 mDecompressedRotNumBytes, mDecompressedPosNumBytes, mDecompressedScaleNumBytes;
    uint32 mNumRotTracks, mNumScaleRotTracks, mNumScaleTracks, mNumPosTracks;
    uint32 mChunkOverhead, mCompressedSize, mOptimizedSize, mUncompressedSize;
    uint32 mScaleRotOffset, mNumSubMotions;
    float  mPosQuantFactor, mRotQuantFactor, mScaleQuantFactor;
    float  mSampleSpacing, mSecondsPerChunk, mMaxTime;
    uint8  mWaveletID, mCompressorID;  // +2 pad
};
static_assert(sizeof(WaveletInfo) == 88);

struct WaveletSkeletalSubMotion {
    File16BitQuaternion mPoseRot, mBindPoseRot, mPoseScaleRot, mBindPoseScaleRot;
    FileVector3 mPosePos, mPoseScale, mBindPosePos, mBindPoseScale;
    float mMaxError;
};
static_assert(sizeof(WaveletSkeletalSubMotion) == 84); // 4*8 (quat16) + 4*12 (vec3) + 4 (maxError)

struct WaveletChunk {
    float mRotQuantScale, mPosQuantScale, mScaleQuantScale, mStartTime;
    uint32 mCompressedRotNumBytes, mCompressedPosNumBytes, mCompressedScaleNumBytes;
    uint32 mCompressedPosNumBits, mCompressedRotNumBits, mCompressedScaleNumBits;
};
static_assert(sizeof(WaveletChunk) == 40);

} // namespace xsm

// ===========================================================================
// XPM — progressive morph motion (facial / morph-target weight animation)
// ===========================================================================
namespace xpm {

enum : uint32 {
    CHUNK_SUBMOTION        = 100,
    CHUNK_INFO             = 101,
    CHUNK_MOTIONEVENTTABLE = SHARED_CHUNK_MOTIONEVENTTABLE,
    CHUNK_SUBMOTIONS       = 102,
};

struct Info { uint32 mMotionFPS; uint8 mExporterHighVersion, mExporterLowVersion; }; // +2 pad
static_assert(sizeof(Info) == 8);

struct ProgressiveSubMotion {
    float  mPoseWeight;   // pose weight when no animation data present
    float  mMinWeight;    // used to unpack the 16-bit keyframe weights
    float  mMaxWeight;
    uint32 mPhonemeSet;   // 0 for a normal progressive morph target
    uint32 mNumKeys;
    // + string name + UnsignedShortKey[mNumKeys]
};
static_assert(sizeof(ProgressiveSubMotion) == 20);

struct FloatKey { float mTime; float mValue; };
struct UnsignedShortKey { float mTime; uint16 mValue; }; // +2 pad
static_assert(sizeof(FloatKey) == 8);
static_assert(sizeof(UnsignedShortKey) == 8);

struct SubMotions { uint32 mNumSubMotions; };

} // namespace xpm

} // namespace tos::emfx::fmt
