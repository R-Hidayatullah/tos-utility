// XAC (EMotionFX actor) parser — skeleton nodes, meshes, materials, skinning.
//
// Ported from the tosmole Rust reference (src/xac.rs) but corrected against the
// authoritative EMotionFX 3.9 SDK struct layouts. Fixed structs are read as
// whole blocks (padding included); trailing strings/arrays are read explicitly.
#pragma once

#include "tos/emfx/emfx_format.h"
#include "tos/emfx/chunk_audit.h"
#include "tos/io/byte_reader.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tos::emfx {

struct XacNode {
    std::string name;
    fmt::FileQuaternion localQuat{};
    fmt::FileQuaternion scaleRot{};
    fmt::FileVector3 localPos{};
    fmt::FileVector3 localScale{};
    fmt::FileVector3 shear{};
    uint32_t skeletalLODs = 0;
    uint32_t parentIndex = 0xFFFFFFFF;
    int version = 0;
};

struct XacSubMesh {
    uint32_t numIndices = 0, numVerts = 0, materialIndex = 0, numBones = 0;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> bones;
};

struct XacVertexLayer {
    uint32_t layerTypeId = 0;
    uint32_t attribSizeInBytes = 0;
    uint8_t enableDeformations = 0;
    uint8_t isScale = 0;
    std::vector<uint8_t> data; // attribSizeInBytes * mesh.totalVerts
};

struct XacMesh {
    uint32_t nodeIndex = 0, lod = 0, numOrgVerts = 0, totalVerts = 0, totalIndices = 0;
    bool isCollisionMesh = false;
    std::vector<XacVertexLayer> layers;
    std::vector<XacSubMesh> subMeshes;
    int version = 0;
};

struct XacStdMaterialLayer {
    std::string texture;
    float amount = 0, uOffset = 0, vOffset = 0, uTiling = 0, vTiling = 0, rotationRadians = 0;
    uint16_t materialNumber = 0;
    uint8_t mapType = 0;
    uint8_t blendMode = 0;
    int version = 0;
};

struct XacStdMaterial {
    std::string name;
    fmt::FileColor ambient{}, diffuse{}, specular{}, emissive{};
    float shine = 0, shineStrength = 0, opacity = 0, ior = 0;
    uint8_t doubleSided = 0, wireFrame = 0, transparencyType = 0;
    uint32_t lod = 0;
    std::vector<XacStdMaterialLayer> layers;
    int version = 0;
};

struct XacFxMaterial {
    std::string name;
    std::string effectFile;
    std::string shaderTechnique;
    uint32_t lod = 0;
    std::vector<std::string> bitmapNames;   // texture references
    std::vector<std::string> bitmapValues;
    // Named effect parameters (blend/alpha/two-sided flags live here).
    std::vector<std::pair<std::string, int32_t>> intParams;
    std::vector<std::pair<std::string, float>> floatParams;
    std::vector<std::pair<std::string, uint8_t>> boolParams;
    int version = 0;
};

struct XacSkinInfluence { float weight = 0; uint16_t nodeNr = 0; };
struct XacSkinning {
    uint32_t nodeIndex = 0;
    bool isForCollisionMesh = false;
    // Per original vertex: list of influences (rebuilt from the flat array + table for v2+).
    std::vector<std::vector<XacSkinInfluence>> perVertex;
    int version = 0;
};

struct XacNodeGroup {
    std::string name;
    bool disabledOnDefault = false;
    std::vector<uint16_t> nodes;
};

// Summary of one progressive-morph mesh-delta block (geometry deltas retained
// as raw counts; full delta arrays can be added when a consumer needs them).
struct XacMorphDelta {
    uint32_t nodeIndex = 0;
    float minValue = 0, maxValue = 0;
    uint32_t numVertices = 0;
};

struct XacMorphTarget {
    std::string name;
    float rangeMin = 0, rangeMax = 0;
    uint32_t lod = 0;
    uint32_t phonemeSets = 0;
    std::vector<XacMorphDelta> deltas;
    uint32_t numTransformations = 0;
    int version = 0;
};

struct XacMeshLodLevel {
    uint32_t lodLevel = 0;
    std::vector<uint8_t> modelData; // embedded LOD memory file
};

// One entry in the actor's global material table, in file-declaration order.
// A submesh's materialIndex indexes this list; isFx selects which vector idx
// points into. StdMaterial and FxMaterial chunks are interleaved in the file,
// so this order must be preserved to map submeshes to the right material.
struct XacMaterialRef { bool isFx = false; uint32_t idx = 0; };

struct XacActor {
    fmt::FileHeader header{};
    std::vector<XacNode> nodes;
    std::vector<XacMesh> meshes;
    std::vector<XacStdMaterial> stdMaterials;
    std::vector<XacFxMaterial> fxMaterials;
    std::vector<XacMaterialRef> materialRefs; // global material index -> source
    std::vector<XacSkinning> skinnings;
    std::vector<XacNodeGroup> nodeGroups;
    std::vector<XacMorphTarget> morphTargets;
    std::vector<XacMeshLodLevel> meshLodLevels;
    std::vector<ChunkAudit> audits;

    std::vector<std::string> textureNames() const;
    bool allChunksExact() const;
};

// Parse an in-memory .xac. Throws std::runtime_error on a fatal/structural error.
XacActor parseXac(const uint8_t* data, size_t size);
XacActor parseXacFile(const std::string& path);

} // namespace tos::emfx
