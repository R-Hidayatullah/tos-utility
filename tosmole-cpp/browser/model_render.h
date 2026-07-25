// modelgfx — a minimal D3D11 mesh renderer for XAC model preview.
//
// Separate device/swapchain from the 2D texture preview (gw2gfx), bound to its
// own child window. Renders one baked (world-space) mesh with an orbit camera,
// a single directional light, and an optional diffuse texture. Bind-pose only
// for now (no GPU skinning yet).
#pragma once

#include <cstddef>
#include <cstdint>
#include <windows.h>

namespace modelgfx {

struct Vertex {
    float px, py, pz;
    float nx, ny, nz;
    float u, v;
    float bone[4];    // up to 4 bone indices (as floats for a simple input layout)
    float weight[4];  // matching influence weights (sum ~1)
};

bool initialize(HWND target_window);
void shutdown();
void on_resize(int width, int height);

// Begin a new baked (already world-space) mesh + its bounding sphere for the
// camera to auto-frame. Replaces any previous model; add draw groups after.
void begin_model(const Vertex* verts, size_t vcount,
                 const uint32_t* indices, size_t icount,
                 float centerX, float centerY, float centerZ, float radius);

// Per-group material render state (mirrors the ToS FX material switches).
struct GroupState {
    bool alphaBlend = false; // src-alpha over; drawn after opaque, no depth write
    bool alphaTest = false;  // clip texels with alpha < alphaRef
    bool zwriteOff = false;  // opaque but don't write depth
    bool lightOff = false;   // use texture colour unlit (map geometry is pre-lit)
    float alphaRef = 0.5f;
};

// One draw group = a contiguous index range with its own diffuse texture
// (BCn or RGBA subresource). data==nullptr renders that group untextured
// (flat white). Call once per submesh/material. alphaBlend groups are drawn in a
// second, alpha-blended pass (depth-tested but not depth-writing).
void add_group(uint32_t index_offset, uint32_t index_count,
               uint32_t dxgi_format, uint32_t width, uint32_t height,
               uint32_t sys_mem_pitch, const uint8_t* data, GroupState state = {},
               uint32_t mip_count = 1);

void clear_model();

// Upload the per-bone skinning matrices for this frame (row-major, count*16
// floats). Pass identity matrices for bind pose. Bones index into this array.
void upload_bones(const float* matrices, int count);

// Orbit camera controls (radians / multiplicative zoom).
void orbit(float d_yaw, float d_pitch);
void zoom(float factor);

// Display toggles (see browser toolbar).
void set_wireframe(bool on);     // overlay a wireframe pass on top of the mesh
void set_show_mesh(bool on);     // draw the shaded solid mesh
void set_use_texture(bool on);   // sample diffuse textures vs. flat shading
void set_show_skeleton(bool on); // overlay the bone skeleton as lines

// Upload the current skeleton as a line list (pairs of endpoints, count = number
// of vertices = 2 * number of bone segments; xyz per vertex, model space).
void upload_skeleton_lines(const float* xyz, int vertex_count);

// --- Particle billboards (map .3deffect preview) --------------------------
// Camera-facing textured quads placed in world space. Rebuilt to face the orbit
// camera every frame; drawn after the mesh, depth-tested but not depth-writing.
// Clear the particle set + its texture cache (call before adding a map's effects).
void begin_particles();
// Register a particle texture (RGBA/BCn subresource); returns a texId to pass to
// add_particle, or -1 on failure. Cache by source on the caller side to reuse.
int  add_particle_texture(uint32_t dxgi_format, uint32_t width, uint32_t height,
                          uint32_t sys_mem_pitch, const uint8_t* data, uint32_t mip_count = 1);
// Place one billboard: world position, world-space half-size, rgba tint, texId,
// and blend mode (additive glow vs. src-alpha over).
void add_particle(float x, float y, float z, float size,
                  float r, float g, float b, float a, int texId, bool additive);
void set_show_particles(bool on);

// Linear distance fog (from a map's .3drender <LinearFog>). When enabled the
// scene fades to `color` between `start` and `end` world units from the camera,
// and the background clears to the fog colour. Disable for single-model preview.
void set_fog(bool enable, float r, float g, float b, float start, float end);
// Toggle fog on/off without discarding the stored colour/range (for a UI toggle).
void set_fog_enabled(bool on);

void render();

} // namespace modelgfx
