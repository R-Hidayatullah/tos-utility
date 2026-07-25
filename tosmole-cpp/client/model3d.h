// model3d (m3d) — a skinned XAC model renderer that shares cgfx's D3D11 device
// and swap-chain, so 3D characters/props draw into the same frame as the 2D UI.
//
// Supports multiple independent Model instances (a character, map geometry, …).
// Vertices are the modelgfx::Vertex produced by browser/xac_geometry.cpp; bones
// are uploaded as skin matrices (invBind*animWorld) — identity == bind pose.
#pragma once

#include "model_render.h"   // modelgfx::Vertex

#include <cstddef>
#include <cstdint>

namespace m3d {

bool init();
void shutdown();

struct GroupState {
    bool alphaBlend = false, alphaTest = false, zwriteOff = false, lightOff = false;
    float alphaRef = 0.5f;
};

struct Model;  // opaque

Model* create();
void   destroy(Model*);

void set_geometry(Model*, const modelgfx::Vertex* v, size_t vcount,
                  const uint32_t* idx, size_t icount, int boneCount);
void clear_groups(Model*);
void add_group(Model*, uint32_t index_offset, uint32_t index_count,
               uint32_t dxgi_fmt, uint32_t w, uint32_t h, uint32_t pitch,
               const uint8_t* data, uint32_t mip, GroupState st);
// Skin matrices, row-major, count*16 floats. Identity per bone == bind pose.
void upload_bones(Model*, const float* mats, int count);

// Camera shared by subsequent draws: viewProj = view*proj (row-vector clip).
void set_camera(const float* viewProj16, const float* camPos3, const float* lightDir3);
// Draw the model. world16 (row-major) is folded into the VP so a model-space
// mesh is placed/rotated in the world. Pass identity to draw at the origin.
void draw(Model*, const float* world16);

} // namespace m3d
