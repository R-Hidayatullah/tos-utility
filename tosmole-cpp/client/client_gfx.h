// client_gfx (cgfx) — unified D3D11 renderer for the tosclient game shell.
//
// One device + swapchain bound to the main window. Provides a 2D pass drawn
// against a fixed virtual "canvas" (design resolution, default 1920x1080) that
// is letterboxed into the real window, plus TTF text rendered with the game's
// own fonts (imcm_book.ttf / imcm_original.ttf) via GDI-to-texture caching.
//
// Phase 2 will add a 3D pass (skinned XAC models + baked maps) on the same
// device; the 2D API here becomes the UI/HUD overlay.
#pragma once

#include <windows.h>
#include <cstddef>
#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace cgfx {

using Tex = int;  // opaque texture handle; -1 == invalid

enum Align { LEFT = 0, CENTER = 1, RIGHT = 2 };

// --- lifecycle -------------------------------------------------------------
bool init(HWND hwnd);
void shutdown();
void resize(int w, int h);
int  window_w();
int  window_h();

// The design-space canvas that 2D coordinates are expressed in. Letterboxed
// (aspect-preserving) into the window; areas outside are the clear colour.
void  set_canvas(float w, float h);
float canvas_w();
float canvas_h();

// Map a window-pixel point (e.g. mouse) into canvas coordinates.
void window_to_canvas(int px, int py, float* cx, float* cy);

// --- textures --------------------------------------------------------------
// 32-bit RGBA, top-down, tightly packed. Returns a handle or -1.
Tex  create_texture_rgba(int w, int h, const uint8_t* rgba);
// Decode an in-memory image (jpg/png/tga/bmp) into an RGBA texture. -1 on fail.
Tex  load_image(const uint8_t* data, size_t size);
void texture_size(Tex t, int* w, int* h);
void destroy_texture(Tex t);

// --- fonts / text ----------------------------------------------------------
// Register a TTF from memory (parses its family name and adds it as a private
// font). idx 0 is used as the default UI face. Call during startup.
void  add_font(const uint8_t* ttf, size_t size);
float text_measure(const char* utf8, float px, int font = 0);
// Draw UTF-8 text; (x,y) is the anchor per `align` (top-aligned vertically).
void  draw_text(const char* utf8, float x, float y, float px,
                float r, float g, float b, float a, int align = LEFT, int font = 0);

// --- 3D interop ------------------------------------------------------------
// Shared device/context so a 3D pass (model3d) can render into the same target.
ID3D11Device*        device();
ID3D11DeviceContext* context();
// Re-arm the 2D pipeline (shader/blend/no-depth) after a 3D pass has run, before
// drawing UI on top. begin_frame() already calls this; call again after 3D.
void use_2d();
// The letterboxed viewport aspect (canvas_w/canvas_h), for the 3D projection.
float viewport_aspect();

// --- 2D primitives (canvas space, origin top-left) -------------------------
// begin_frame clears colour + depth, binds the render target + depth buffer,
// sets the letterbox viewport, then arms the 2D pipeline.
void begin_frame(float r, float g, float b);
void end_frame();

void draw_rect(float x, float y, float w, float h,
               float r, float g, float b, float a);
void draw_sprite(Tex t, float x, float y, float w, float h,
                 float r = 1, float g = 1, float b = 1, float a = 1);
void draw_sprite_uv(Tex t, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    float r, float g, float b, float a);

} // namespace cgfx
