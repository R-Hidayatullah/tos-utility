#include "xac_geometry.h"
#include "tinymath.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace tosb {

using namespace tos::emfx;

namespace {

// XAC vertex-attribute layer type IDs (XACAttribute).
constexpr uint32_t ATTR_POSITIONS      = 0;
constexpr uint32_t ATTR_NORMALS        = 1;
constexpr uint32_t ATTR_UVCOORDS       = 3;
constexpr uint32_t ATTR_ORGVTXNUMBERS  = 5;
constexpr uint8_t  XAC_LAYERID_DIFFUSE = 2;

const XacVertexLayer* findLayer(const XacMesh& m, uint32_t typeId) {
    for (const auto& l : m.layers)
        if (l.layerTypeId == typeId) return &l;
    return nullptr;
}

std::string stdMaterialTexture(const XacStdMaterial& m) {
    for (const auto& l : m.layers)
        if (l.mapType == XAC_LAYERID_DIFFUSE && !l.texture.empty()) return l.texture;
    for (const auto& l : m.layers)
        if (!l.texture.empty()) return l.texture;
    return {};
}
std::string fxMaterialTexture(const XacFxMaterial& m) {
    for (const auto& v : m.bitmapValues)
        if (!v.empty() && v != "None") return v;
    return {};
}
// Render flags for one material, mirroring the ToS FX material g_is* switches.
struct MatFlags {
    bool alphaBlend = false, alphaTest = false, zwriteOff = false, lightOff = false;
    float alphaRef = 0.5f;
};

const uint8_t* findBool(const XacFxMaterial& m, const char* name) {
    for (const auto& p : m.boolParams) if (p.first == name) return &p.second;
    return nullptr;
}
MatFlags fxMaterialFlags(const XacFxMaterial& m) {
    MatFlags f;
    if (auto* b = findBool(m, "g_isAlphaBlendOn")) f.alphaBlend = *b != 0;
    if (auto* b = findBool(m, "g_isAlphaTestOn"))  f.alphaTest  = *b != 0;
    if (auto* b = findBool(m, "g_isZwriteOff"))    f.zwriteOff  = *b != 0;
    if (auto* b = findBool(m, "g_isLightOff"))     f.lightOff   = *b != 0;
    for (const auto& p : m.intParams)
        if (p.first == "g_alphaTestValue") f.alphaRef = std::clamp(p.second / 255.0f, 0.0f, 1.0f);
    return f;
}
MatFlags stdMaterialFlags(const XacStdMaterial& m) {
    MatFlags f;
    // Standard materials (character models etc.): stay lit; treat clearly
    // translucent ones as alpha-blended.
    if (m.opacity > 0.0f && m.opacity < 0.99f) f.alphaBlend = true;
    return f;
}
std::vector<MatFlags> materialFlags(const XacActor& a) {
    std::vector<MatFlags> out;
    if (!a.materialRefs.empty()) {
        out.reserve(a.materialRefs.size());
        for (const auto& ref : a.materialRefs) {
            if (ref.isFx && ref.idx < a.fxMaterials.size()) out.push_back(fxMaterialFlags(a.fxMaterials[ref.idx]));
            else if (!ref.isFx && ref.idx < a.stdMaterials.size()) out.push_back(stdMaterialFlags(a.stdMaterials[ref.idx]));
            else out.push_back({});
        }
        return out;
    }
    for (const auto& m : a.stdMaterials) out.push_back(stdMaterialFlags(m));
    for (const auto& m : a.fxMaterials)  out.push_back(fxMaterialFlags(m));
    return out;
}

std::vector<std::string> materialTextures(const XacActor& a) {
    std::vector<std::string> t;
    // Follow the file-declaration order (std/fx chunks are interleaved) so each
    // entry lines up with the submesh materialIndex.
    if (!a.materialRefs.empty()) {
        t.reserve(a.materialRefs.size());
        for (const auto& ref : a.materialRefs) {
            if (ref.isFx && ref.idx < a.fxMaterials.size())
                t.push_back(fxMaterialTexture(a.fxMaterials[ref.idx]));
            else if (!ref.isFx && ref.idx < a.stdMaterials.size())
                t.push_back(stdMaterialTexture(a.stdMaterials[ref.idx]));
            else
                t.push_back({});
        }
        return t;
    }
    // Fallback for actors parsed without a material table (older path).
    for (const auto& m : a.stdMaterials) t.push_back(stdMaterialTexture(m));
    for (const auto& m : a.fxMaterials)  t.push_back(fxMaterialTexture(m));
    return t;
}

// Build the skeleton with bind-pose world + inverse-bind per node.
std::vector<XacBone> buildBones(const XacActor& a) {
    std::vector<XacBone> bones(a.nodes.size());
    for (size_t i = 0; i < a.nodes.size(); ++i) {
        const XacNode& n = a.nodes[i];
        XacBone& b = bones[i];
        b.name = n.name;
        b.parent = (n.parentIndex < a.nodes.size()) ? (int)n.parentIndex : -1;
        b.localPos = {n.localPos.mX, n.localPos.mY, n.localPos.mZ};
        b.localScale = {n.localScale.mX, n.localScale.mY, n.localScale.mZ};
        b.localRot[0] = n.localQuat.mX; b.localRot[1] = n.localQuat.mY;
        b.localRot[2] = n.localQuat.mZ; b.localRot[3] = n.localQuat.mW;
        tmath::Mat4 S = tmath::scaling(b.localScale.x, b.localScale.y, b.localScale.z);
        tmath::Mat4 R = tmath::rotationQuat(b.localRot[0], b.localRot[1], b.localRot[2], b.localRot[3]);
        tmath::Mat4 T = tmath::translation(b.localPos.x, b.localPos.y, b.localPos.z);
        tmath::Mat4 local = tmath::mul(tmath::mul(S, R), T);
        b.bindWorld = (b.parent >= 0) ? tmath::mul(local, bones[b.parent].bindWorld) : local;
        b.invBind = tmath::inverse(b.bindWorld);
    }
    return bones;
}

const XacSkinning* findSkinning(const XacActor& a, uint32_t nodeIndex) {
    for (const auto& s : a.skinnings)
        if (s.nodeIndex == nodeIndex) return &s;
    return nullptr;
}

// Fill a vertex's up-to-4 bone indices + normalized weights from an influence list.
void fillSkin(modelgfx::Vertex& out, const std::vector<XacSkinInfluence>& infl, uint32_t fallbackBone) {
    for (int k = 0; k < 4; ++k) { out.bone[k] = 0; out.weight[k] = 0; }
    if (infl.empty()) { out.bone[0] = (float)fallbackBone; out.weight[0] = 1.0f; return; }
    // top 4 by weight
    std::array<std::pair<float, uint32_t>, 4> top{};
    int n = 0;
    for (const auto& i : infl) {
        if (n < 4) { top[n++] = {i.weight, i.nodeNr}; }
        else {
            int mi = 0; for (int k = 1; k < 4; ++k) if (top[k].first < top[mi].first) mi = k;
            if (i.weight > top[mi].first) top[mi] = {i.weight, i.nodeNr};
        }
    }
    float sum = 0; for (int k = 0; k < n; ++k) sum += top[k].first;
    if (sum <= 0) { out.bone[0] = (float)fallbackBone; out.weight[0] = 1.0f; return; }
    for (int k = 0; k < n; ++k) { out.bone[k] = (float)top[k].second; out.weight[k] = top[k].first / sum; }
}

} // namespace

XacGeometry buildXacGeometry(const XacActor& actor, bool bakeNodeTransforms) {
    XacGeometry g;
    g.bones = buildBones(actor);
    auto matTex = materialTextures(actor);
    auto matFlags = materialFlags(actor);

    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};

    for (const auto& mesh : actor.meshes) {
        if (mesh.isCollisionMesh) continue;
        const XacVertexLayer* posL = findLayer(mesh, ATTR_POSITIONS);
        if (!posL || mesh.totalVerts == 0) continue;
        const XacVertexLayer* nrmL = findLayer(mesh, ATTR_NORMALS);
        const XacVertexLayer* uvL  = findLayer(mesh, ATTR_UVCOORDS);
        const XacVertexLayer* orgL = findLayer(mesh, ATTR_ORGVTXNUMBERS);
        const XacSkinning* skin = findSkinning(actor, mesh.nodeIndex);
        if (skin) g.anySkinned = true;
        ++g.meshCount;

        // Skinned meshes store vertices in MODEL space already — do NOT bake the
        // mesh-node transform (that offsets meshes whose node isn't the root, e.g.
        // the alpaca's saddle). Rigid meshes store node-local verts -> bake to model.
        tmath::Mat4 world = tmath::identity();
        if (bakeNodeTransforms && !skin && mesh.nodeIndex < g.bones.size())
            world = g.bones[mesh.nodeIndex].bindWorld;

        const uint32_t baseVertex = (uint32_t)g.verts.size();
        const uint32_t vn = mesh.totalVerts;
        g.verts.reserve(g.verts.size() + vn);

        for (uint32_t v = 0; v < vn; ++v) {
            modelgfx::Vertex out{};
            float p[3] = {0, 0, 0};
            std::memcpy(p, posL->data.data() + (size_t)v * posL->attribSizeInBytes, sizeof(float) * 3);
            tmath::Vec3 pw = tmath::transformPoint({p[0], p[1], p[2]}, world);
            out.px = pw.x; out.py = pw.y; out.pz = pw.z;

            if (nrmL) {
                float nrm[3] = {0, 1, 0};
                std::memcpy(nrm, nrmL->data.data() + (size_t)v * nrmL->attribSizeInBytes, sizeof(float) * 3);
                tmath::Vec3 nw = tmath::transformNormal({nrm[0], nrm[1], nrm[2]}, world);
                out.nx = nw.x; out.ny = nw.y; out.nz = nw.z;
            } else { out.nx = 0; out.ny = 1; out.nz = 0; }

            if (uvL) {
                float uv[2] = {0, 0};
                std::memcpy(uv, uvL->data.data() + (size_t)v * uvL->attribSizeInBytes, sizeof(float) * 2);
                out.u = uv[0]; out.v = uv[1];
            }

            // Skinning influences: map render vertex -> original vertex -> influences.
            if (skin && orgL) {
                uint32_t org = 0;
                std::memcpy(&org, orgL->data.data() + (size_t)v * orgL->attribSizeInBytes, sizeof(uint32_t));
                if (org < skin->perVertex.size()) fillSkin(out, skin->perVertex[org], mesh.nodeIndex);
                else fillSkin(out, {}, mesh.nodeIndex);
            } else {
                fillSkin(out, {}, mesh.nodeIndex);  // rigid: single bone = mesh node
            }
            g.verts.push_back(out);

            mn[0] = std::min(mn[0], out.px); mx[0] = std::max(mx[0], out.px);
            mn[1] = std::min(mn[1], out.py); mx[1] = std::max(mx[1], out.py);
            mn[2] = std::min(mn[2], out.pz); mx[2] = std::max(mx[2], out.pz);
        }

        uint32_t subStart = 0;
        for (const auto& sm : mesh.subMeshes) {
            XacDrawGroup grp;
            grp.indexOffset = (uint32_t)g.indices.size();
            for (uint32_t idx : sm.indices)
                g.indices.push_back(baseVertex + subStart + idx);
            grp.indexCount = (uint32_t)sm.indices.size();
            if (sm.materialIndex < matTex.size()) grp.textureName = matTex[sm.materialIndex];
            if (sm.materialIndex < matFlags.size()) {
                const MatFlags& f = matFlags[sm.materialIndex];
                grp.alphaBlend = f.alphaBlend; grp.alphaTest = f.alphaTest;
                grp.zwriteOff = f.zwriteOff; grp.lightOff = f.lightOff; grp.alphaRef = f.alphaRef;
            }
            g.groups.push_back(std::move(grp));
            subStart += sm.numVerts;
        }
    }

    if (g.verts.empty() || g.indices.empty()) return g;

    for (int k = 0; k < 3; ++k) g.center[k] = 0.5f * (mn[k] + mx[k]);
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    g.radius = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
    if (g.radius < 1e-4f) g.radius = 1.0f;

    g.ok = true;
    return g;
}

} // namespace tosb
