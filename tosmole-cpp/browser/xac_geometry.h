// Build renderable geometry from a parsed XAC actor (bind pose).
#pragma once

#include "model_render.h"
#include "tinymath.h"
#include "tos/emfx/xac.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tosb {

// One contiguous index range with its own diffuse texture (by basename) plus the
// render flags from its material (ToS FX material g_is* bools / alpha-test ref).
struct XacDrawGroup {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    std::string textureName;
    bool alphaBlend = false; // g_isAlphaBlendOn: src-alpha over, no depth write
    bool alphaTest = false;  // g_isAlphaTestOn: clip texels below alphaRef
    bool zwriteOff = false;  // g_isZwriteOff
    bool lightOff = false;   // g_isLightOff: use texture colour unlit
    float alphaRef = 0.5f;   // g_alphaTestValue / 255
};

// One skeleton bone (= XAC node) with its bind-pose transforms.
struct XacBone {
    std::string name;
    int parent = -1;
    tmath::Vec3 localPos;
    tmath::Vec3 localScale{1, 1, 1};
    float localRot[4] = {0, 0, 0, 1}; // bind local rotation quat (x,y,z,w)
    tmath::Mat4 bindWorld;            // bind-pose world matrix
    tmath::Mat4 invBind;              // inverse(bindWorld)
};

struct XacGeometry {
    std::vector<modelgfx::Vertex> verts;
    std::vector<uint32_t> indices;
    std::vector<XacDrawGroup> groups;  // per-submesh (grouped by material)
    std::vector<XacBone> bones;        // skeleton (index == node index)
    float center[3] = {0, 0, 0};
    float radius = 1.0f;
    bool ok = false;
    int meshCount = 0;
    bool anySkinned = false;
};

// bakeNodeTransforms: fold each rigid mesh's bind-pose node transform into its
// vertices. Correct for standalone models (character preview, multi-node parts).
// For .3dworld placement pass false — those models are authored in model space
// and positioned by the map's per-instance pos/rot/scale, so baking the node
// transform on top double-transforms them (e.g. floating props).
XacGeometry buildXacGeometry(const tos::emfx::XacActor& actor, bool bakeNodeTransforms = true);

} // namespace tosb
