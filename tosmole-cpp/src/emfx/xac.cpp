#include "tos/emfx/xac.h"

#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace tos::emfx {

namespace {
using namespace tos::emfx::fmt;
using tos::io::ByteReader;

std::string readStr(ByteReader& r) { return r.read_string(); }

XacNode readNodeCommon(ByteReader& r, int version) {
    XacNode n;
    n.version = version;
    // The first 5 transform members are identical across all node versions.
    n.localQuat  = r.read<FileQuaternion>();
    n.scaleRot   = r.read<FileQuaternion>();
    n.localPos   = r.read<FileVector3>();
    n.localScale = r.read<FileVector3>();
    n.shear      = r.read<FileVector3>();
    return n;
}

// --- individual chunk readers (each consumes exactly its structural data) ---

XacNode readNode4(ByteReader& r); // defined below

void readNodeChunk(ByteReader& r, uint32_t version, XacActor& out) {
    // Single-node chunk (XAC_CHUNK_NODE). Name follows the fixed struct.
    switch (version) {
        case 1: {
            auto n = readNodeCommon(r, 1);
            n.skeletalLODs = r.read_u32();
            n.parentIndex  = r.read_u32();
            n.name = readStr(r);
            out.nodes.push_back(std::move(n));
            break;
        }
        case 2: {
            auto n = readNodeCommon(r, 2);
            n.skeletalLODs = r.read_u32();
            n.parentIndex  = r.read_u32();
            r.read_u8();               // mNodeFlags
            r.skip(3);                 // padding
            n.name = readStr(r);
            out.nodes.push_back(std::move(n));
            break;
        }
        case 3: {
            auto n = readNodeCommon(r, 3);
            n.skeletalLODs = r.read_u32();
            n.parentIndex  = r.read_u32();
            r.read_u8();               // mNodeFlags
            r.skip(3);                 // padding BEFORE mOBB
            r.skip(16 * sizeof(float));// mOBB[16]
            n.name = readStr(r);
            out.nodes.push_back(std::move(n));
            break;
        }
        case 4:
            out.nodes.push_back(readNode4(r));
            break;
        default:
            throw std::runtime_error("unsupported XAC node version");
    }
}

XacNode readNode4(ByteReader& r) {
    auto n = readNodeCommon(r, 4);
    n.skeletalLODs = r.read_u32();
    r.read_u32();                  // mMotionLODs
    n.parentIndex = r.read_u32();
    r.read_u32();                  // mNumChilds
    r.read_u8();                   // mNodeFlags
    r.skip(3);                     // padding BEFORE mOBB
    r.skip(16 * sizeof(float));    // mOBB[16]
    r.read_f32();                  // mImportanceFactor
    n.name = readStr(r);
    return n;
}

void readNodeGroupChunk(ByteReader& r, XacActor& out) {
    // Fixed header read as a block (uint16 + uint8 + 1 pad), per the SDK
    // block-read convention. NOTE: no ToS test file exercises this chunk, so
    // the trailing-pad assumption is unverified against real data — the
    // mSizeInBytes oracle will flag it if a real file disagrees.
    auto hdr = r.read<xac::NodeGroup>();
    XacNodeGroup g;
    g.disabledOnDefault = hdr.mDisabledOnDefault != 0;
    g.name = readStr(r);
    g.nodes = r.read_array<uint16_t>(hdr.mNumNodes);
    out.nodeGroups.push_back(std::move(g));
}

void readMeshLodChunk(ByteReader& r, XacActor& out) {
    XacMeshLodLevel lod;
    lod.lodLevel = r.read_u32();
    uint32_t sizeInBytes = r.read_u32();
    lod.modelData = r.read_bytes(sizeInBytes);
    out.meshLodLevels.push_back(std::move(lod));
}

// One XAC_PMorphTarget: fixed header + name + mesh-delta blocks + transforms.
XacMorphTarget readMorphTarget(ByteReader& r, int version) {
    auto h = r.read<xac::PMorphTarget>();
    XacMorphTarget mt;
    mt.version = version;
    mt.rangeMin = h.mRangeMin;
    mt.rangeMax = h.mRangeMax;
    mt.lod = h.mLOD;
    mt.phonemeSets = h.mPhonemeSets;
    mt.numTransformations = h.mNumTransformations;
    mt.name = readStr(r);
    mt.deltas.reserve(h.mNumMeshDeformDeltas);
    for (uint32_t d = 0; d < h.mNumMeshDeformDeltas; ++d) {
        auto md = r.read<xac::PMorphTargetMeshDeltas>();
        XacMorphDelta delta{md.mNodeIndex, md.mMinValue, md.mMaxValue, md.mNumVertices};
        // delta position (16-bit v3), normals (8-bit v3), tangents (8-bit v3),
        // then original vertex numbers (uint32).
        r.skip(static_cast<size_t>(md.mNumVertices) * sizeof(fmt::File16BitVector3));
        r.skip(static_cast<size_t>(md.mNumVertices) * sizeof(fmt::File8BitVector3));
        r.skip(static_cast<size_t>(md.mNumVertices) * sizeof(fmt::File8BitVector3));
        r.skip(static_cast<size_t>(md.mNumVertices) * sizeof(uint32_t));
        mt.deltas.push_back(delta);
    }
    for (uint32_t t = 0; t < h.mNumTransformations; ++t)
        r.read<xac::PMorphTargetTransform>();
    return mt;
}

// Bundled morph-target chunk (XAC_CHUNK_STDPMORPHTARGETS, id 12):
// header {numMorphTargets, lod} followed by that many XAC_PMorphTarget.
void readMorphTargetsChunk(ByteReader& r, XacActor& out) {
    auto h = r.read<xac::PMorphTargets>();
    for (uint32_t i = 0; i < h.mNumMorphTargets; ++i)
        out.morphTargets.push_back(readMorphTarget(r, 1));
}

void readNodesChunk(ByteReader& r, XacActor& out) {
    uint32_t numNodes = r.read_u32();
    r.read_u32(); // mNumRootNodes
    out.nodes.reserve(out.nodes.size() + numNodes);
    for (uint32_t i = 0; i < numNodes; ++i)
        out.nodes.push_back(readNode4(r));
}

XacMesh readMesh(ByteReader& r, uint32_t version) {
    XacMesh m;
    m.version = static_cast<int>(version);
    m.nodeIndex = r.read_u32();
    if (version >= 2) m.lod = r.read_u32();
    m.numOrgVerts  = r.read_u32();
    m.totalVerts   = r.read_u32();
    m.totalIndices = r.read_u32();
    uint32_t numSubMeshes = r.read_u32();
    uint32_t numLayers    = r.read_u32();
    m.isCollisionMesh = r.read_u8() != 0;
    r.skip(3); // padding

    m.layers.reserve(numLayers);
    for (uint32_t i = 0; i < numLayers; ++i) {
        XacVertexLayer layer;
        layer.layerTypeId       = r.read_u32();
        layer.attribSizeInBytes = r.read_u32();
        layer.enableDeformations= r.read_u8();
        layer.isScale           = r.read_u8();
        r.skip(2); // padding
        layer.data = r.read_bytes(static_cast<size_t>(layer.attribSizeInBytes) * m.totalVerts);
        m.layers.push_back(std::move(layer));
    }
    m.subMeshes.reserve(numSubMeshes);
    for (uint32_t i = 0; i < numSubMeshes; ++i) {
        XacSubMesh sm;
        sm.numIndices    = r.read_u32();
        sm.numVerts      = r.read_u32();
        sm.materialIndex = r.read_u32();
        sm.numBones      = r.read_u32();
        sm.indices = r.read_array<uint32_t>(sm.numIndices);
        sm.bones   = r.read_array<uint32_t>(sm.numBones);
        m.subMeshes.push_back(std::move(sm));
    }
    return m;
}

// Skinning: parsed within the chunk's byte bounds so we don't need the
// owning mesh's org-vertex count as cross-chunk state.
void readSkinning(ByteReader& r, uint32_t version, uint32_t declaredSize, size_t chunkStart, XacActor& out) {
    XacSkinning sk;
    sk.version = static_cast<int>(version);
    const size_t end = chunkStart + declaredSize;

    if (version == 1) {
        sk.nodeIndex = r.read_u32();
        sk.isForCollisionMesh = r.read_u8() != 0;
        // (SkinningInfo is 8 bytes: 4 + 1 + 3 pad)
        r.skip(3);
        while (r.tell() < end) {
            uint8_t numInf = r.read_u8();
            std::vector<XacSkinInfluence> infs;
            infs.reserve(numInf);
            for (uint8_t i = 0; i < numInf; ++i) {
                auto raw = r.read<xac::SkinInfluence>();
                infs.push_back({raw.mWeight, raw.mNodeNr});
            }
            sk.perVertex.push_back(std::move(infs));
        }
    } else {
        uint32_t numTotalInfluences = 0;
        if (version == 2) {
            sk.nodeIndex = r.read_u32();
            numTotalInfluences = r.read_u32();
            sk.isForCollisionMesh = r.read_u8() != 0;
            r.skip(3);
        } else if (version == 3) {
            sk.nodeIndex = r.read_u32();
            r.read_u32(); // mNumLocalBones
            numTotalInfluences = r.read_u32();
            sk.isForCollisionMesh = r.read_u8() != 0;
            r.skip(3);
        } else { // version 4
            sk.nodeIndex = r.read_u32();
            r.read_u32(); // mLOD
            r.read_u32(); // mNumLocalBones
            numTotalInfluences = r.read_u32();
            sk.isForCollisionMesh = r.read_u8() != 0;
            r.skip(3);
        }
        std::vector<XacSkinInfluence> flat;
        flat.reserve(numTotalInfluences);
        for (uint32_t i = 0; i < numTotalInfluences; ++i) {
            auto raw = r.read<xac::SkinInfluence>();
            flat.push_back({raw.mWeight, raw.mNodeNr});
        }
        // Remaining bytes are SkinningInfoTableEntry[numOrgVerts] (8 bytes each).
        size_t tableBytes = end - r.tell();
        size_t numOrgVerts = tableBytes / sizeof(xac::SkinningInfoTableEntry);
        sk.perVertex.reserve(numOrgVerts);
        for (size_t v = 0; v < numOrgVerts; ++v) {
            auto te = r.read<xac::SkinningInfoTableEntry>();
            std::vector<XacSkinInfluence> infs;
            infs.reserve(te.mNumElements);
            for (uint32_t i = 0; i < te.mNumElements; ++i) {
                size_t idx = te.mStartIndex + i;
                if (idx < flat.size()) infs.push_back(flat[idx]);
            }
            sk.perVertex.push_back(std::move(infs));
        }
    }
    out.skinnings.push_back(std::move(sk));
}

XacStdMaterialLayer readStdMatLayer(ByteReader& r, uint32_t version) {
    XacStdMaterialLayer l;
    l.version = static_cast<int>(version);
    l.amount = r.read_f32();
    l.uOffset = r.read_f32();
    l.vOffset = r.read_f32();
    l.uTiling = r.read_f32();
    l.vTiling = r.read_f32();
    l.rotationRadians = r.read_f32();
    l.materialNumber = r.read_u16();
    l.mapType = r.read_u8();
    if (version >= 2) l.blendMode = r.read_u8();
    else r.skip(1); // padding
    l.texture = readStr(r);
    return l;
}

void readStdMaterial(ByteReader& r, uint32_t version, XacActor& out) {
    XacStdMaterial m;
    m.version = static_cast<int>(version);
    uint32_t numLayers = 0;
    if (version == 3) m.lod = r.read_u32();
    m.ambient  = r.read<FileColor>();
    m.diffuse  = r.read<FileColor>();
    m.specular = r.read<FileColor>();
    m.emissive = r.read<FileColor>();
    m.shine = r.read_f32();
    m.shineStrength = r.read_f32();
    m.opacity = r.read_f32();
    m.ior = r.read_f32();
    m.doubleSided = r.read_u8();
    m.wireFrame = r.read_u8();
    m.transparencyType = r.read_u8();
    if (version == 1) {
        r.skip(1); // padding
    } else {
        numLayers = r.read_u8();
    }
    m.name = readStr(r);
    for (uint32_t i = 0; i < numLayers; ++i)
        m.layers.push_back(readStdMatLayer(r, 2)); // v2+ materials embed v2 layers
    out.stdMaterials.push_back(std::move(m));
    out.materialRefs.push_back({false, (uint32_t)out.stdMaterials.size() - 1});
}

// Standalone material-layer chunk (XAC_CHUNK_STDMATERIALLAYER, older files).
void readStdMaterialLayerChunk(ByteReader& r, uint32_t version, XacActor& out) {
    auto l = readStdMatLayer(r, version);
    // Attach to the most recent standard material, if any.
    if (!out.stdMaterials.empty())
        out.stdMaterials.back().layers.push_back(std::move(l));
}

void readFxMaterial(ByteReader& r, uint32_t version, XacActor& out) {
    XacFxMaterial m;
    m.version = static_cast<int>(version);
    uint32_t numInt=0, numFloat=0, numColor=0, numBool=0, numVec3=0, numBitmap=0;
    if (version == 1) {
        numInt = r.read_u32(); numFloat = r.read_u32(); numColor = r.read_u32(); numBitmap = r.read_u32();
    } else {
        if (version == 3) m.lod = r.read_u32();
        numInt = r.read_u32(); numFloat = r.read_u32(); numColor = r.read_u32();
        numBool = r.read_u32(); numVec3 = r.read_u32(); numBitmap = r.read_u32();
    }
    m.name = readStr(r);
    m.effectFile = readStr(r);
    if (version >= 2) m.shaderTechnique = readStr(r);

    for (uint32_t i = 0; i < numInt; ++i)   { int32_t v = r.read_i32(); m.intParams.push_back({readStr(r), v}); }
    for (uint32_t i = 0; i < numFloat; ++i) { float v = r.read_f32(); m.floatParams.push_back({readStr(r), v}); }
    for (uint32_t i = 0; i < numColor; ++i) { r.read<FileColor>(); readStr(r); }
    for (uint32_t i = 0; i < numBool; ++i)  { uint8_t v = r.read_u8(); m.boolParams.push_back({readStr(r), v}); }
    for (uint32_t i = 0; i < numVec3; ++i)  { r.read<FileVector3>(); readStr(r); }
    for (uint32_t i = 0; i < numBitmap; ++i) {
        m.bitmapNames.push_back(readStr(r));
        m.bitmapValues.push_back(readStr(r));
    }
    out.fxMaterials.push_back(std::move(m));
    out.materialRefs.push_back({true, (uint32_t)out.fxMaterials.size() - 1});
}

} // namespace

XacActor parseXac(const uint8_t* data, size_t size) {
    ByteReader r(data, size);
    XacActor actor;
    actor.header = r.read<FileHeader>();
    if (!(actor.header.mFourcc[0] == 'X' && actor.header.mFourcc[1] == 'A' &&
          actor.header.mFourcc[2] == 'C' && actor.header.mFourcc[3] == ' '))
        throw std::runtime_error("not an XAC file (bad fourcc)");

    while (r.remaining() >= sizeof(FileChunk)) {
        FileChunk chunk = r.read<FileChunk>();
        const size_t start = r.tell();
        const size_t declaredEnd = start + chunk.mSizeInBytes;
        if (declaredEnd > size) break; // truncated / trailing garbage

        ChunkAudit audit;
        audit.id = chunk.mChunkID;
        audit.version = chunk.mVersion;
        audit.declaredSize = chunk.mSizeInBytes;
        audit.handled = true;

        try {
            switch (chunk.mChunkID) {
                case xac::CHUNK_INFO:
                    // Fixed struct + 4 strings; struct size varies by version.
                    switch (chunk.mVersion) {
                        case 1: r.read<xac::Info>();  break;
                        case 2: r.read<xac::Info2>(); break;
                        case 3: r.read<xac::Info3>(); break;
                        case 4: r.read<xac::Info4>(); break;
                        default: audit.handled = false; break;
                    }
                    if (audit.handled) { for (int i=0;i<4;++i) readStr(r); }
                    audit.label = "Info";
                    break;
                case xac::CHUNK_NODE:
                    readNodeChunk(r, chunk.mVersion, actor); audit.label = "Node"; break;
                case xac::CHUNK_NODES:
                    readNodesChunk(r, actor); audit.label = "Nodes"; break;
                case xac::CHUNK_MESH:
                    actor.meshes.push_back(readMesh(r, chunk.mVersion)); audit.label = "Mesh"; break;
                case xac::CHUNK_SKINNINGINFO:
                    readSkinning(r, chunk.mVersion, chunk.mSizeInBytes, start, actor); audit.label = "SkinningInfo"; break;
                case xac::CHUNK_STDMATERIAL:
                    readStdMaterial(r, chunk.mVersion, actor); audit.label = "StdMaterial"; break;
                case xac::CHUNK_STDMATERIALLAYER:
                    readStdMaterialLayerChunk(r, chunk.mVersion, actor); audit.label = "StdMaterialLayer"; break;
                case xac::CHUNK_FXMATERIAL:
                    readFxMaterial(r, chunk.mVersion, actor); audit.label = "FxMaterial"; break;
                case xac::CHUNK_MATERIALINFO:
                    if (chunk.mVersion == 1) r.read<xac::MaterialInfo>();
                    else if (chunk.mVersion == 2) r.read<xac::MaterialInfo2>();
                    else audit.handled = false;
                    audit.label = "MaterialInfo"; break;
                case xac::CHUNK_LIMIT:
                    r.read<xac::Limit>(); audit.label = "Limit"; break;
                case xac::CHUNK_NODEGROUPS:
                    readNodeGroupChunk(r, actor); audit.label = "NodeGroup"; break;
                case xac::CHUNK_MESHLODLEVELS:
                    readMeshLodChunk(r, actor); audit.label = "MeshLODLevel"; break;
                case xac::CHUNK_STDPROGMORPHTARGET:
                    actor.morphTargets.push_back(readMorphTarget(r, 1)); audit.label = "PMorphTarget"; break;
                case xac::CHUNK_STDPMORPHTARGETS:
                    readMorphTargetsChunk(r, actor); audit.label = "PMorphTargets"; break;
                case xac::CHUNK_NODEMOTIONSOURCES: {
                    uint32_t n = r.read_u32(); r.read_array<uint16_t>(n); audit.label = "NodeMotionSources"; break;
                }
                case xac::CHUNK_ATTACHMENTNODES: {
                    uint32_t n = r.read_u32(); r.read_array<uint16_t>(n); audit.label = "AttachmentNodes"; break;
                }
                default:
                    audit.handled = false;
                    audit.label = "?";
                    break;
            }
        } catch (const std::exception&) {
            audit.handled = false;
        }

        audit.consumedSize = static_cast<uint32_t>(r.tell() - start);
        audit.exact = audit.handled && (r.tell() == declaredEnd);
        audit.overRead = audit.handled && (r.tell() > declaredEnd);
        actor.audits.push_back(audit);

        // Realignment policy:
        //  - handled chunk that consumed >= declared: trust the structural
        //    position. ToS's exporter under-counts mSizeInBytes for standard
        //    material chunks (the embedded layers extend past the declared
        //    end), so rewinding to declaredEnd would desync the stream.
        //  - handled chunk that under-read, or an unhandled chunk: skip to
        //    the declared end.
        if (r.tell() < declaredEnd)
            r.seek(declaredEnd);
    }
    return actor;
}

XacActor parseXacFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + path);
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parseXac(buf.data(), buf.size());
}

std::vector<std::string> XacActor::textureNames() const {
    std::vector<std::string> names;
    for (const auto& m : stdMaterials)
        for (const auto& l : m.layers)
            if (!l.texture.empty()) names.push_back(l.texture);
    for (const auto& m : fxMaterials)
        for (const auto& v : m.bitmapValues)
            if (!v.empty()) names.push_back(v);
    return names;
}

bool XacActor::allChunksExact() const {
    // A handled chunk is "clean" if it consumed exactly the declared size, or
    // legitimately over-read past an under-counted size (std-material layers).
    for (const auto& a : audits)
        if (a.handled && !a.exact && !a.overRead) return false;
    return true;
}

} // namespace tos::emfx
