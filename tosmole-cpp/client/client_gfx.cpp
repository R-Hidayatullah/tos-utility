#include "client_gfx.h"
#include "stb_image.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace cgfx {
namespace {

using Microsoft::WRL::ComPtr;

HWND g_hwnd = nullptr;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11Texture2D> g_depthTex;
ComPtr<ID3D11DepthStencilView> g_dsv;
ComPtr<ID3D11DepthStencilState> g_dss2d;   // depth disabled (2D overlay)
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<ID3D11InputLayout> g_layout;
ComPtr<ID3D11Buffer> g_vb;      // dynamic quad (6 verts) rebuilt per draw
ComPtr<ID3D11Buffer> g_cb;      // canvas size
ComPtr<ID3D11BlendState> g_blend;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11RasterizerState> g_raster;

int g_winW = 1, g_winH = 1;
float g_canvasW = 1920.f, g_canvasH = 1080.f;
// Letterbox mapping (window px = canvas*scale + offset).
float g_scale = 1.f, g_offX = 0.f, g_offY = 0.f;

struct Texture { ComPtr<ID3D11ShaderResourceView> srv; int w = 0, h = 0; };
std::vector<Texture> g_textures;
Tex g_white = -1;

struct Vtx { float x, y, u, v, r, g, b, a; };

// --- constant buffer (canvas size for the ortho transform) -----------------
struct CB { float cw, ch, pad0, pad1; };

constexpr char kShader[] = R"(
cbuffer CB : register(b0) { float2 canvas; float2 pad; };
struct VSIn  { float2 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
VSOut VSMain(VSIn i){
    VSOut o;
    float2 ndc = float2(i.pos.x / canvas.x * 2.0 - 1.0,
                        1.0 - i.pos.y / canvas.y * 2.0);
    o.pos = float4(ndc, 0, 1);
    o.uv = i.uv; o.col = i.col; return o;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(VSOut i):SV_TARGET{
    float4 t = tex0.Sample(samp0, i.uv);
    return t * i.col;
}
)";

void recompute_letterbox() {
    float sx = (float)g_winW / g_canvasW;
    float sy = (float)g_winH / g_canvasH;
    g_scale = sx < sy ? sx : sy;
    g_offX = (g_winW - g_canvasW * g_scale) * 0.5f;
    g_offY = (g_winH - g_canvasH * g_scale) * 0.5f;
}

bool create_rtv() {
    ComPtr<ID3D11Texture2D> back;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)back.GetAddressOf())))
        return false;
    return SUCCEEDED(g_device->CreateRenderTargetView(back.Get(), nullptr, g_rtv.ReleaseAndGetAddressOf()));
}

void create_depth(int w, int h) {
    g_dsv.Reset(); g_depthTex.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, g_depthTex.GetAddressOf())))
        g_device->CreateDepthStencilView(g_depthTex.Get(), nullptr, g_dsv.GetAddressOf());
}

Tex make_texture(int w, int h, const uint8_t* rgba) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = rgba; sd.SysMemPitch = w * 4;
    ComPtr<ID3D11Texture2D> t;
    if (FAILED(g_device->CreateTexture2D(&td, &sd, t.GetAddressOf()))) return -1;
    Texture out; out.w = w; out.h = h;
    if (FAILED(g_device->CreateShaderResourceView(t.Get(), nullptr, out.srv.GetAddressOf())))
        return -1;
    g_textures.push_back(std::move(out));
    return (Tex)g_textures.size() - 1;
}

// ---------------------------------------------------------------------------
// GDI text via the game fonts
// ---------------------------------------------------------------------------
std::vector<std::wstring> g_fontFaces;         // by font index
std::vector<HANDLE>       g_fontHandles;        // AddFontMemResourceEx handles
HDC g_memdc = nullptr;

uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Pull the font family name (name table, nameID=1) out of a TTF so we can
// CreateFontW against a private font added from memory.
std::wstring ttf_family(const uint8_t* d, size_t n) {
    if (n < 12) return L"";
    uint16_t numTables = be16(d + 4);
    const uint8_t* rec = d + 12;
    const uint8_t* nameTab = nullptr;
    for (int i = 0; i < numTables && (size_t)((rec - d) + 16) <= n; ++i, rec += 16) {
        if (std::memcmp(rec, "name", 4) == 0) { nameTab = d + be32(rec + 8); break; }
    }
    if (!nameTab || (size_t)(nameTab - d) + 6 > n) return L"";
    uint16_t count = be16(nameTab + 2);
    uint16_t strOff = be16(nameTab + 4);
    const uint8_t* nr = nameTab + 6;
    std::wstring best;
    for (int i = 0; i < count; ++i, nr += 12) {
        uint16_t platform = be16(nr + 0);
        uint16_t nameID = be16(nr + 6);
        uint16_t len = be16(nr + 8);
        uint16_t off = be16(nr + 10);
        if (nameID != 1) continue;
        const uint8_t* s = nameTab + strOff + off;
        if ((size_t)(s - d) + len > n) continue;
        if (platform == 3 || platform == 0) {         // UTF-16BE
            std::wstring w;
            for (int j = 0; j + 1 < len; j += 2) w.push_back((wchar_t)be16(s + j));
            if (!w.empty()) { best = w; break; }
        } else if (platform == 1 && best.empty()) {    // Mac ASCII
            std::wstring w;
            for (int j = 0; j < len; ++j) w.push_back((wchar_t)s[j]);
            best = w;
        }
    }
    return best;
}

std::map<std::pair<int, int>, HFONT> g_hfonts;  // (face, px) -> HFONT
HFONT get_hfont(int font, int px) {
    auto key = std::make_pair(font, px);
    auto it = g_hfonts.find(key);
    if (it != g_hfonts.end()) return it->second;
    const wchar_t* face = (font >= 0 && font < (int)g_fontFaces.size() && !g_fontFaces[font].empty())
                              ? g_fontFaces[font].c_str() : L"Malgun Gothic";
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                          ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    g_hfonts[key] = f;
    return f;
}

std::wstring to_wide(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    return w;
}

struct Glyphs { Tex tex; int w, h; };
std::map<std::string, Glyphs> g_textCache;

Glyphs rasterize(const std::wstring& w, int font, int px) {
    HFONT hf = get_hfont(font, px);
    HGDIOBJ old = SelectObject(g_memdc, hf);
    SIZE sz{};
    GetTextExtentPoint32W(g_memdc, w.c_str(), (int)w.size(), &sz);
    int tw = sz.cx + 2, th = sz.cy + 2;
    if (tw < 1) tw = 1; if (th < 1) th = 1;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = tw;
    bmi.bmiHeader.biHeight = -th;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(g_memdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbmp = SelectObject(g_memdc, dib);
    std::memset(bits, 0, (size_t)tw * th * 4);   // black background
    SetBkMode(g_memdc, TRANSPARENT);
    SetTextColor(g_memdc, RGB(255, 255, 255));
    TextOutW(g_memdc, 1, 1, w.c_str(), (int)w.size());
    GdiFlush();

    // Luminance -> alpha, rgb = white (tinted at draw time).
    std::vector<uint8_t> rgba((size_t)tw * th * 4);
    const uint8_t* src = (const uint8_t*)bits;
    for (int i = 0; i < tw * th; ++i) {
        uint8_t a = src[i * 4];  // B channel (grayscale => any channel equal)
        rgba[i * 4 + 0] = 255; rgba[i * 4 + 1] = 255; rgba[i * 4 + 2] = 255; rgba[i * 4 + 3] = a;
    }
    Glyphs g{make_texture(tw, th, rgba.data()), tw, th};

    SelectObject(g_memdc, oldbmp);
    DeleteObject(dib);
    SelectObject(g_memdc, old);
    return g;
}

Glyphs& cached_text(const char* utf8, int font, int px) {
    std::string key = std::to_string(font) + "|" + std::to_string(px) + "|" + utf8;
    auto it = g_textCache.find(key);
    if (it != g_textCache.end()) return it->second;
    return g_textCache[key] = rasterize(to_wide(utf8), font, px);
}

void draw_quad(Tex t, float x, float y, float w, float h,
               float u0, float v0, float u1, float v1,
               float r, float g, float b, float a) {
    if (t < 0 || t >= (Tex)g_textures.size()) return;
    Vtx v[6] = {
        {x,     y,     u0, v0, r, g, b, a}, {x + w, y,     u1, v0, r, g, b, a},
        {x,     y + h, u0, v1, r, g, b, a}, {x + w, y,     u1, v0, r, g, b, a},
        {x + w, y + h, u1, v1, r, g, b, a}, {x,     y + h, u0, v1, r, g, b, a},
    };
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) return;
    std::memcpy(ms.pData, v, sizeof(v));
    g_ctx->Unmap(g_vb.Get(), 0);
    UINT stride = sizeof(Vtx), off = 0;
    g_ctx->IASetVertexBuffers(0, 1, g_vb.GetAddressOf(), &stride, &off);
    ID3D11ShaderResourceView* srv = g_textures[t].srv.Get();
    g_ctx->PSSetShaderResources(0, 1, &srv);
    g_ctx->Draw(6, 0);
}

} // namespace

// ---------------------------------------------------------------------------
bool init(HWND hwnd) {
    g_hwnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    g_winW = rc.right - rc.left; g_winH = rc.bottom - rc.top;
    if (g_winW < 1) g_winW = 1; if (g_winH < 1) g_winH = 1;

    DXGI_SWAP_CHAIN_DESC sc{};
    sc.BufferCount = 2;
    sc.BufferDesc.Width = g_winW; sc.BufferDesc.Height = g_winH;
    sc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.OutputWindow = hwnd; sc.SampleDesc.Count = 1; sc.Windowed = TRUE;
    sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            flags, nullptr, 0, D3D11_SDK_VERSION, &sc, g_swap.GetAddressOf(),
            g_device.GetAddressOf(), &fl, g_ctx.GetAddressOf())))
        return false;
    if (!create_rtv()) return false;
    create_depth(g_winW, g_winH);

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
            "VSMain", "vs_4_0", 0, 0, vsb.GetAddressOf(), err.GetAddressOf())))
        return false;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, nullptr, nullptr, nullptr,
            "PSMain", "ps_4_0", 0, 0, psb.GetAddressOf(), err.ReleaseAndGetAddressOf())))
        return false;
    g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, g_vs.GetAddressOf());
    g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, g_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_device->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), g_layout.GetAddressOf());

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(Vtx) * 6; vbd.Usage = D3D11_USAGE_DYNAMIC;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER; vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&vbd, nullptr, g_vb.GetAddressOf());

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CB); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_device->CreateBuffer(&cbd, nullptr, g_cb.GetAddressOf());

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bd, g_blend.GetAddressOf());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, g_sampler.GetAddressOf());

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rd, g_raster.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = FALSE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_device->CreateDepthStencilState(&dd, g_dss2d.GetAddressOf());

    g_memdc = CreateCompatibleDC(nullptr);

    uint8_t whitepx[4] = {255, 255, 255, 255};
    g_white = make_texture(1, 1, whitepx);

    recompute_letterbox();
    return true;
}

void shutdown() {
    for (auto& kv : g_hfonts) if (kv.second) DeleteObject(kv.second);
    g_hfonts.clear();
    if (g_memdc) { DeleteDC(g_memdc); g_memdc = nullptr; }
    for (HANDLE h : g_fontHandles) if (h) RemoveFontMemResourceEx(h);
    g_fontHandles.clear();
}

void resize(int w, int h) {
    if (w < 1 || h < 1 || !g_swap) return;
    g_winW = w; g_winH = h;
    g_rtv.Reset();
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv();
    create_depth(w, h);
    recompute_letterbox();
}

ID3D11Device* device() { return g_device.Get(); }
ID3D11DeviceContext* context() { return g_ctx.Get(); }
float viewport_aspect() { return g_canvasW / g_canvasH; }

int window_w() { return g_winW; }
int window_h() { return g_winH; }
void set_canvas(float w, float h) { g_canvasW = w; g_canvasH = h; recompute_letterbox(); }
float canvas_w() { return g_canvasW; }
float canvas_h() { return g_canvasH; }

void window_to_canvas(int px, int py, float* cx, float* cy) {
    *cx = (px - g_offX) / g_scale;
    *cy = (py - g_offY) / g_scale;
}

Tex create_texture_rgba(int w, int h, const uint8_t* rgba) { return make_texture(w, h, rgba); }

Tex load_image(const uint8_t* data, size_t size) {
    int w, h, ch;
    stbi_uc* px = stbi_load_from_memory(data, (int)size, &w, &h, &ch, 4);
    if (!px) return -1;
    Tex t = make_texture(w, h, px);
    stbi_image_free(px);
    return t;
}

void texture_size(Tex t, int* w, int* h) {
    if (t < 0 || t >= (Tex)g_textures.size()) { *w = *h = 0; return; }
    *w = g_textures[t].w; *h = g_textures[t].h;
}

void destroy_texture(Tex) { /* pooled; freed at shutdown */ }

void add_font(const uint8_t* ttf, size_t size) {
    DWORD nfonts = 0;
    HANDLE h = AddFontMemResourceEx((void*)ttf, (DWORD)size, nullptr, &nfonts);
    if (h) g_fontHandles.push_back(h);
    g_fontFaces.push_back(ttf_family(ttf, size));
}

float text_measure(const char* utf8, float px, int font) {
    Glyphs& g = cached_text(utf8, font, (int)(px + 0.5f));
    return (float)g.w;
}

void draw_text(const char* utf8, float x, float y, float px,
               float r, float g, float b, float a, int align, int font) {
    if (!utf8 || !*utf8) return;
    Glyphs& gl = cached_text(utf8, font, (int)(px + 0.5f));
    float dx = x;
    if (align == CENTER) dx = x - gl.w * 0.5f;
    else if (align == RIGHT) dx = x - gl.w;
    draw_quad(gl.tex, dx, y, (float)gl.w, (float)gl.h, 0, 0, 1, 1, r, g, b, a);
}

void use_2d() {
    CB cb{g_canvasW, g_canvasH, 0, 0};
    g_ctx->UpdateSubresource(g_cb.Get(), 0, nullptr, &cb, 0, 0);
    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
    g_ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    float bf[4] = {0, 0, 0, 0};
    g_ctx->OMSetBlendState(g_blend.Get(), bf, 0xffffffff);
    g_ctx->OMSetDepthStencilState(g_dss2d.Get(), 0);
    g_ctx->RSSetState(g_raster.Get());
}

void begin_frame(float r, float g, float b) {
    ID3D11RenderTargetView* rtv = g_rtv.Get();
    g_ctx->OMSetRenderTargets(1, &rtv, g_dsv.Get());
    float cc[4] = {r, g, b, 1.f};
    g_ctx->ClearRenderTargetView(g_rtv.Get(), cc);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.f, 0);

    // Viewport = letterboxed canvas rect.
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = g_offX; vp.TopLeftY = g_offY;
    vp.Width = g_canvasW * g_scale; vp.Height = g_canvasH * g_scale;
    vp.MinDepth = 0; vp.MaxDepth = 1;
    g_ctx->RSSetViewports(1, &vp);

    use_2d();
}

void end_frame() { g_swap->Present(1, 0); }

void draw_rect(float x, float y, float w, float h, float r, float g, float b, float a) {
    draw_quad(g_white, x, y, w, h, 0, 0, 1, 1, r, g, b, a);
}
void draw_sprite(Tex t, float x, float y, float w, float h, float r, float g, float b, float a) {
    draw_quad(t, x, y, w, h, 0, 0, 1, 1, r, g, b, a);
}
void draw_sprite_uv(Tex t, float x, float y, float w, float h,
                    float u0, float v0, float u1, float v1,
                    float r, float g, float b, float a) {
    draw_quad(t, x, y, w, h, u0, v0, u1, v1, r, g, b, a);
}

} // namespace cgfx
