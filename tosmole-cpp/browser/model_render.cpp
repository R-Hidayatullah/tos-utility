#include "model_render.h"
#include "tinymath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace modelgfx {
namespace {

using Microsoft::WRL::ComPtr;

HWND g_hwnd = nullptr;
ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_rtv;
ComPtr<ID3D11Texture2D> g_depth;
ComPtr<ID3D11DepthStencilView> g_dsv;
ComPtr<ID3D11DepthStencilState> g_dss;
ComPtr<ID3D11DepthStencilState> g_dssReadOnly;  // transparent pass: test, no write
ComPtr<ID3D11BlendState> g_blendOpaque, g_blendAlpha;
ComPtr<ID3D11RasterizerState> g_rasterSolid, g_rasterWire;
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<ID3D11InputLayout> g_layout;
ComPtr<ID3D11Buffer> g_vb, g_ib, g_cb;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11ShaderResourceView> g_white;  // 1x1 white fallback for untextured groups
ComPtr<ID3D11Buffer> g_boneBuf;            // StructuredBuffer<float4x4> of skin matrices
ComPtr<ID3D11ShaderResourceView> g_boneSRV;
int g_boneCap = 0, g_boneCount = 0;

// Skeleton overlay (line list of bone->parent segments).
ComPtr<ID3D11VertexShader> g_skelVS;
ComPtr<ID3D11PixelShader> g_skelPS;
ComPtr<ID3D11InputLayout> g_skelLayout;
ComPtr<ID3D11Buffer> g_skelVB;
ComPtr<ID3D11DepthStencilState> g_dssNoDepth;
int g_skelVBCap = 0, g_skelVertCount = 0;

struct DrawGroup { UINT offset = 0, count = 0; ComPtr<ID3D11ShaderResourceView> srv;
                   GroupState st; };
std::vector<DrawGroup> g_groups;

// Particle billboards (map effect preview).
ComPtr<ID3D11VertexShader> g_bbVS;
ComPtr<ID3D11PixelShader> g_bbPS;
ComPtr<ID3D11InputLayout> g_bbLayout;
ComPtr<ID3D11Buffer> g_bbVB;            // dynamic, rebuilt each frame (camera-facing)
ComPtr<ID3D11BlendState> g_blendAdd;    // additive (ONE, ONE) glow
int g_bbVBCap = 0;
bool g_showParticles = true;
struct Billboard { float pos[3]; float size; float col[4]; int texId; bool additive; };
std::vector<Billboard> g_billboards;
std::vector<ComPtr<ID3D11ShaderResourceView>> g_particleTex;
struct BBVert { float px, py, pz; float u, v; float r, g, b, a; };

UINT g_indexCount = 0;
int g_w = 1, g_h = 1;
bool g_wire = false;         // wireframe overlay pass
bool g_showMesh = true;      // draw the shaded solid mesh
bool g_useTex = true;        // sample diffuse textures
bool g_showSkeleton = false; // draw the bone skeleton

// Camera (orbit around target).
tmath::Vec3 g_target{0, 0, 0};
float g_yaw = 0.6f, g_pitch = 0.35f, g_dist = 5.0f;
float g_radius = 1.0f;  // bounding-sphere radius of the current model (for near/far)

// Linear distance fog (map .3drender). Disabled for single-model preview.
bool g_fogEnable = false;
float g_fogColor[3] = {0.1f, 0.11f, 0.13f};
float g_fogStart = 0, g_fogEnd = 0;

struct CB {
    tmath::Mat4 viewProj;   // row-major (HLSL uses row_major + mul(v, M))
    float lightDir[3]; float useTex;
    float wireMode;         // wireMode>0.5 -> flat wire colour
    float alphaTestRef;     // >=0 -> clip(alpha - ref); <0 -> no alpha test
    float lightOff;         // >0.5 -> unlit (use texture colour directly)
    float fogEnable;        // >0.5 -> apply linear distance fog
    float fogColor[3]; float fogStart;
    float camPos[3];   float fogEnd;
    float fogBias;     float _pad2[3];  // shift fog to the orbit camera's near framing
};

constexpr char kShader[] = R"(
cbuffer CB : register(b0) {
    row_major float4x4 viewProj;
    float3 lightDir; float useTex;
    float wireMode; float alphaTestRef; float lightOff; float fogEnable;
    float3 fogColor; float fogStart;
    float3 camPos;   float fogEnd;
    float fogBias;   float3 _pad2;
};
struct Bone { row_major float4x4 m; };
StructuredBuffer<Bone> gBones : register(t1);
struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD;
               float4 bone:BLENDINDICES; float4 weight:BLENDWEIGHT; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD; float3 wpos:TEXCOORD1; };
VSOut VSMain(VSIn i) {
    // Blend the influencing bone (skin) matrices, then project. Vertices are in
    // model/bind space; each bone matrix = invBind * animWorld (row-vector).
    float4x4 skin = gBones[(uint)i.bone.x].m * i.weight.x
                  + gBones[(uint)i.bone.y].m * i.weight.y
                  + gBones[(uint)i.bone.z].m * i.weight.z
                  + gBones[(uint)i.bone.w].m * i.weight.w;
    float3 sp = mul(float4(i.pos, 1.0), skin).xyz;
    float3 sn = mul(float4(i.nrm, 0.0), skin).xyz;
    VSOut o;
    o.pos = mul(float4(sp, 1.0), viewProj);
    o.nrm = sn;
    o.uv  = i.uv;
    o.wpos = sp;
    return o;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(VSOut i) : SV_TARGET {
    if (wireMode > 0.5) return float4(0.15, 1.0, 0.45, 1.0); // wireframe overlay colour
    float4 t = tex0.Sample(samp0, i.uv);
    float a = (useTex > 0.5) ? t.a : 1.0;
    if (alphaTestRef >= 0.0) clip(a - alphaTestRef); // alpha-tested cutout / drop clear texels
    float3 base = (useTex > 0.5) ? t.rgb : float3(0.75, 0.76, 0.80);
    float3 col;
    // Map geometry is pre-lit (g_isLightOff): show the texture directly. When the
    // texture toggle is off, always shade so the untextured form is readable.
    if (lightOff > 0.5 && useTex > 0.5) {
        col = base;
    } else {
        float ndl = saturate(dot(normalize(i.nrm), -normalize(lightDir)));
        col = base * (0.30 + 0.70 * ndl);
    }
    // Linear distance fog (map .3drender): fade to fogColor from start..end.
    if (fogEnable > 0.5) {
        float dist = max(0.0, length(i.wpos - camPos) - fogBias);
        float f = saturate((fogEnd - dist) / max(1.0, fogEnd - fogStart));
        col = lerp(fogColor, col, f);
    }
    return float4(col, a);
}
)";

// Skeleton overlay: transform bare positions by viewProj, draw a flat colour.
constexpr char kSkelShader[] = R"(
cbuffer CB : register(b0) {
    row_major float4x4 viewProj;
    float3 lightDir; float useTex;
    float wireMode; float3 _pad;
};
float4 VSMain(float3 pos : POSITION) : SV_POSITION {
    return mul(float4(pos, 1.0), viewProj);
}
float4 PSMain() : SV_TARGET { return float4(1.0, 0.85, 0.15, 1.0); }
)";

// Particle billboards: quads pre-expanded on the CPU to face the camera; the VS
// just projects, the PS modulates the sprite texture by the per-vertex tint.
// Reuses the model CB (only its viewProj is read).
constexpr char kBillboardShader[] = R"(
cbuffer CB : register(b0) {
    row_major float4x4 viewProj;
    float3 lightDir; float useTex;
    float wireMode; float alphaTestRef; float lightOff; float _pad;
};
struct VSIn  { float3 pos:POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD; float4 col:COLOR; };
VSOut VSMain(VSIn i) {
    VSOut o;
    o.pos = mul(float4(i.pos, 1.0), viewProj);
    o.uv = i.uv; o.col = i.col;
    return o;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(VSOut i) : SV_TARGET {
    return tex0.Sample(samp0, i.uv) * i.col;
}
)";

void createRTV() {
    g_rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb))))
        g_device->CreateRenderTargetView(bb.Get(), nullptr, &g_rtv);
}

void createDepth(int w, int h) {
    g_dsv.Reset(); g_depth.Reset();
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &g_depth)))
        g_device->CreateDepthStencilView(g_depth.Get(), nullptr, &g_dsv);
}

bool createPipeline() {
    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "model", nullptr, nullptr,
                          "VSMain", "vs_4_0", 0, 0, &vsb, &err))) return false;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "model", nullptr, nullptr,
                          "PSMain", "ps_4_0", 0, 0, &psb, &err))) return false;
    g_device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_device->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_device->CreateInputLayout(il, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CB); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_device->CreateBuffer(&cbd, nullptr, &g_cb);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    g_device->CreateDepthStencilState(&dsd, &g_dss);

    // Depth-disabled state for the skeleton overlay (always visible through the mesh).
    D3D11_DEPTH_STENCIL_DESC nd{};
    nd.DepthEnable = FALSE; nd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    nd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    g_device->CreateDepthStencilState(&nd, &g_dssNoDepth);

    // Transparent pass: test against the opaque depth but don't write it (so
    // blended surfaces don't occlude each other by draw order).
    D3D11_DEPTH_STENCIL_DESC rd2{};
    rd2.DepthEnable = TRUE; rd2.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    rd2.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    g_device->CreateDepthStencilState(&rd2, &g_dssReadOnly);

    // Blend states: opaque (overwrite) and standard src-alpha over.
    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].BlendEnable = FALSE;
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bo, &g_blendOpaque);
    D3D11_BLEND_DESC ba{};
    ba.RenderTarget[0].BlendEnable = TRUE;
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&ba, &g_blendAlpha);

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_device->CreateRasterizerState(&rd, &g_rasterSolid);
    // Wireframe overlay: bias depth toward the camera so the lines sit on top of
    // the shaded surface instead of z-fighting with it.
    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.DepthBias = -800; rd.SlopeScaledDepthBias = -1.0f; rd.DepthBiasClamp = 0.0f;
    g_device->CreateRasterizerState(&rd, &g_rasterWire);

    // Skeleton line pipeline (position-only, flat colour).
    ComPtr<ID3DBlob> svb, spb, serr;
    if (SUCCEEDED(D3DCompile(kSkelShader, sizeof(kSkelShader) - 1, "skel", nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &svb, &serr)) &&
        SUCCEEDED(D3DCompile(kSkelShader, sizeof(kSkelShader) - 1, "skel", nullptr, nullptr,
                             "PSMain", "ps_4_0", 0, 0, &spb, &serr))) {
        g_device->CreateVertexShader(svb->GetBufferPointer(), svb->GetBufferSize(), nullptr, &g_skelVS);
        g_device->CreatePixelShader(spb->GetBufferPointer(), spb->GetBufferSize(), nullptr, &g_skelPS);
        D3D11_INPUT_ELEMENT_DESC sil[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_device->CreateInputLayout(sil, 1, svb->GetBufferPointer(), svb->GetBufferSize(), &g_skelLayout);
    }

    // Additive blend (ONE, ONE) for glow-style particles.
    D3D11_BLEND_DESC bad{};
    bad.RenderTarget[0].BlendEnable = TRUE;
    bad.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bad.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bad.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bad.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bad.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bad.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bad.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_device->CreateBlendState(&bad, &g_blendAdd);

    // Billboard particle pipeline (position + uv + colour; reuses the model CB).
    ComPtr<ID3DBlob> bvb, bpb, berr;
    if (SUCCEEDED(D3DCompile(kBillboardShader, sizeof(kBillboardShader) - 1, "bb", nullptr, nullptr,
                             "VSMain", "vs_4_0", 0, 0, &bvb, &berr)) &&
        SUCCEEDED(D3DCompile(kBillboardShader, sizeof(kBillboardShader) - 1, "bb", nullptr, nullptr,
                             "PSMain", "ps_4_0", 0, 0, &bpb, &berr))) {
        g_device->CreateVertexShader(bvb->GetBufferPointer(), bvb->GetBufferSize(), nullptr, &g_bbVS);
        g_device->CreatePixelShader(bpb->GetBufferPointer(), bpb->GetBufferSize(), nullptr, &g_bbPS);
        D3D11_INPUT_ELEMENT_DESC bil[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        g_device->CreateInputLayout(bil, 3, bvb->GetBufferPointer(), bvb->GetBufferSize(), &g_bbLayout);
    }

    // Anisotropic + full mip range: trilinear/aniso sampling now that textures
    // carry real mip chains (kills the shimmering on distant/grazing map surfaces).
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MinLOD = 0.0f; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_device->CreateSamplerState(&sd, &g_sampler);
    return g_vs && g_ps && g_layout && g_cb;
}

// defined lower down (same anonymous namespace)
ComPtr<ID3D11ShaderResourceView> makeSRV(uint32_t, uint32_t, uint32_t, uint32_t, const uint8_t*,
                                         uint32_t mipCount = 1);
void ensureWhite();

} // namespace

bool initialize(HWND hwnd) {
    g_hwnd = hwnd;
    RECT rc; GetClientRect(hwnd, &rc);
    g_w = std::max<int>(1, rc.right - rc.left);
    g_h = std::max<int>(1, rc.bottom - rc.top);

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = g_w; scd.BufferDesc.Height = g_h;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd; scd.SampleDesc.Count = 1; scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 1,
                                               D3D11_SDK_VERSION, &scd, &g_swap, &g_device, &got, &g_ctx);
    if (FAILED(hr)) return false;
    createRTV();
    createDepth(g_w, g_h);
    if (!createPipeline()) return false;
    ensureWhite();
    return true;
}

void shutdown() {
    g_groups.clear(); g_white.Reset(); g_boneSRV.Reset(); g_boneBuf.Reset();
    g_billboards.clear(); g_particleTex.clear(); g_bbVB.Reset(); g_bbLayout.Reset();
    g_bbPS.Reset(); g_bbVS.Reset(); g_blendAdd.Reset();
    g_skelVB.Reset(); g_skelLayout.Reset(); g_skelPS.Reset(); g_skelVS.Reset(); g_dssNoDepth.Reset();
    g_dssReadOnly.Reset(); g_blendOpaque.Reset(); g_blendAlpha.Reset();
    g_sampler.Reset(); g_cb.Reset(); g_ib.Reset(); g_vb.Reset();
    g_layout.Reset(); g_ps.Reset(); g_vs.Reset(); g_rasterWire.Reset(); g_rasterSolid.Reset();
    g_dss.Reset(); g_dsv.Reset(); g_depth.Reset(); g_rtv.Reset();
    g_swap.Reset(); g_ctx.Reset(); g_device.Reset(); g_hwnd = nullptr;
}

void on_resize(int w, int h) {
    if (!g_swap || w <= 0 || h <= 0) return;
    g_w = w; g_h = h;
    g_rtv.Reset();
    g_swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    createRTV();
    createDepth(w, h);
}

namespace {
// Block-compressed (BCn) DXGI formats: 4x4 blocks of 8 or 16 bytes.
bool isBlockCompressed(DXGI_FORMAT f) {
    return (f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
           (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB);
}
uint32_t bcBlockBytes(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: case DXGI_FORMAT_BC1_UNORM: case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC4_TYPELESS: case DXGI_FORMAT_BC4_UNORM: case DXGI_FORMAT_BC4_SNORM:
        return 8;
    default: return 16;
    }
}

// Create a shader texture with a full mip chain. Three cases:
//   * mipCount > 1 (the DDS shipped mips): upload every level from `data`.
//   * mipCount <= 1, uncompressed: generate mips on the GPU (fixes shimmering).
//   * mipCount <= 1, block-compressed: single level (can't GPU-generate BCn mips).
ComPtr<ID3D11ShaderResourceView> makeSRV(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch,
                                         const uint8_t* data, uint32_t mipCount) {
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!g_device || !data || !w || !h) return srv;
    DXGI_FORMAT f = (DXGI_FORMAT)fmt;
    bool bc = isBlockCompressed(f);

    if (mipCount <= 1 && !bc) {
        // Runtime mip generation for a single uncompressed level.
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = 0; d.ArraySize = 1;
        d.Format = f; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex))) return srv;
        g_ctx->UpdateSubresource(tex.Get(), 0, nullptr, data, pitch, 0);
        if (SUCCEEDED(g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv)) && srv)
            g_ctx->GenerateMips(srv.Get());
        return srv;
    }

    // Build a subresource per stored mip level (data holds them contiguously).
    if (mipCount < 1) mipCount = 1;
    uint32_t bpp = bc ? 0 : (h ? pitch / w : 4);
    std::vector<D3D11_SUBRESOURCE_DATA> subs(mipCount);
    const uint8_t* p = data;
    uint32_t mw = w, mh = h;
    for (uint32_t i = 0; i < mipCount; ++i) {
        uint32_t rowPitch, levelBytes;
        if (bc) {
            uint32_t bx = (mw + 3) / 4, by = (mh + 3) / 4;
            rowPitch = bx * bcBlockBytes(f);
            levelBytes = rowPitch * by;
        } else {
            rowPitch = mw * bpp;
            levelBytes = rowPitch * mh;
        }
        subs[i].pSysMem = p;
        subs[i].SysMemPitch = rowPitch;
        subs[i].SysMemSlicePitch = 0;
        p += levelBytes;
        mw = mw > 1 ? mw >> 1 : 1;
        mh = mh > 1 ? mh >> 1 : 1;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = mipCount; d.ArraySize = 1;
    d.Format = f; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    if (SUCCEEDED(g_device->CreateTexture2D(&d, subs.data(), &tex)))
        g_device->CreateShaderResourceView(tex.Get(), nullptr, &srv);
    return srv;
}
void ensureWhite() {
    if (g_white) return;
    const uint8_t px[4] = {255, 255, 255, 255};
    g_white = makeSRV(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, px);
}
} // namespace

void begin_model(const Vertex* verts, size_t vcount, const uint32_t* indices, size_t icount,
                 float cx, float cy, float cz, float radius) {
    g_vb.Reset(); g_ib.Reset(); g_indexCount = 0; g_groups.clear();
    if (!g_device || !verts || !vcount || !indices || !icount) return;

    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = (UINT)(vcount * sizeof(Vertex));
    vbd.Usage = D3D11_USAGE_IMMUTABLE; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vsd{verts, 0, 0};
    g_device->CreateBuffer(&vbd, &vsd, &g_vb);

    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = (UINT)(icount * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE; ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA isd{indices, 0, 0};
    g_device->CreateBuffer(&ibd, &isd, &g_ib);

    g_indexCount = (UINT)icount;
    g_target = {cx, cy, cz};
    g_radius = std::max(0.01f, radius);
    g_dist = g_radius * 2.4f;
    g_yaw = 0.6f; g_pitch = 0.35f;
}

void add_group(uint32_t index_offset, uint32_t index_count, uint32_t fmt, uint32_t w, uint32_t h,
               uint32_t pitch, const uint8_t* data, GroupState state, uint32_t mip_count) {
    DrawGroup gp;
    gp.offset = index_offset; gp.count = index_count;
    gp.srv = data ? makeSRV(fmt, w, h, pitch, data, mip_count) : nullptr;
    gp.st = state;
    g_groups.push_back(std::move(gp));
}

void clear_model() { g_vb.Reset(); g_ib.Reset(); g_indexCount = 0; g_groups.clear(); }

void begin_particles() { g_billboards.clear(); g_particleTex.clear(); }

int add_particle_texture(uint32_t fmt, uint32_t w, uint32_t h, uint32_t pitch, const uint8_t* data,
                         uint32_t mip_count) {
    ComPtr<ID3D11ShaderResourceView> srv = makeSRV(fmt, w, h, pitch, data, mip_count);
    if (!srv) return -1;
    g_particleTex.push_back(srv);
    return (int)g_particleTex.size() - 1;
}

void add_particle(float x, float y, float z, float size, float r, float g, float b, float a,
                  int texId, bool additive) {
    if (texId < 0 || texId >= (int)g_particleTex.size()) return;
    Billboard bb;
    bb.pos[0] = x; bb.pos[1] = y; bb.pos[2] = z; bb.size = size;
    bb.col[0] = r; bb.col[1] = g; bb.col[2] = b; bb.col[3] = a;
    bb.texId = texId; bb.additive = additive;
    g_billboards.push_back(bb);
}

void set_show_particles(bool on) { g_showParticles = on; }

void set_fog(bool enable, float r, float g, float b, float start, float end) {
    g_fogEnable = enable && end > start;
    g_fogColor[0] = r; g_fogColor[1] = g; g_fogColor[2] = b;
    g_fogStart = start; g_fogEnd = end;
}

void set_fog_enabled(bool on) { g_fogEnable = on && g_fogEnd > g_fogStart; }

void upload_bones(const float* mats, int count) {
    if (count <= 0 || !g_device) { g_boneCount = 0; return; }
    if (count > g_boneCap) {
        g_boneSRV.Reset(); g_boneBuf.Reset();
        int cap = count + 32;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(cap * 64); bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 64;
        if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_boneBuf))) return;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.FirstElement = 0; sd.Buffer.NumElements = (UINT)cap;
        g_device->CreateShaderResourceView(g_boneBuf.Get(), &sd, &g_boneSRV);
        g_boneCap = cap;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_boneBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, mats, (size_t)count * 64);
        g_ctx->Unmap(g_boneBuf.Get(), 0);
    }
    g_boneCount = count;
}
void orbit(float dy, float dp) {
    g_yaw += dy;
    g_pitch = std::clamp(g_pitch + dp, -1.55f, 1.55f);
}
void zoom(float f) { g_dist = std::clamp(g_dist * f, 0.05f, 1e5f); }
void set_wireframe(bool on) { g_wire = on; }
void set_show_mesh(bool on) { g_showMesh = on; }
void set_use_texture(bool on) { g_useTex = on; }
void set_show_skeleton(bool on) { g_showSkeleton = on; }

void upload_skeleton_lines(const float* xyz, int vertex_count) {
    g_skelVertCount = 0;
    if (!g_device || !xyz || vertex_count <= 0) return;
    if (vertex_count > g_skelVBCap) {
        g_skelVB.Reset();
        int cap = vertex_count + 64;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (UINT)(cap * 3 * sizeof(float));
        bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(g_device->CreateBuffer(&bd, nullptr, &g_skelVB))) return;
        g_skelVBCap = cap;
    }
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_skelVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, xyz, (size_t)vertex_count * 3 * sizeof(float));
        g_ctx->Unmap(g_skelVB.Get(), 0);
        g_skelVertCount = vertex_count;
    }
}

void render() {
    if (!g_ctx || !g_rtv) return;
    // Clear to the fog colour when fog is on so distant geometry fades seamlessly
    // into the background; a neutral dark grey otherwise.
    const float bg[4] = {g_fogEnable ? g_fogColor[0] : 0.11f,
                         g_fogEnable ? g_fogColor[1] : 0.12f,
                         g_fogEnable ? g_fogColor[2] : 0.14f, 1.0f};
    ID3D11RenderTargetView* rtvs[] = {g_rtv.Get()};
    g_ctx->OMSetRenderTargets(1, rtvs, g_dsv.Get());
    g_ctx->ClearRenderTargetView(g_rtv.Get(), bg);
    if (g_dsv) g_ctx->ClearDepthStencilView(g_dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp{0, 0, (float)g_w, (float)g_h, 0, 1};
    g_ctx->RSSetViewports(1, &vp);

    if (g_indexCount && g_vb && g_ib && g_boneSRV) {
        float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
        float cy = std::cos(g_yaw), sy = std::sin(g_yaw);
        tmath::Vec3 eye{g_target.x + g_dist * cp * cy, g_target.y + g_dist * sp,
                     g_target.z + g_dist * cp * sy};
        tmath::Mat4 view = tmath::lookAtLH(eye, g_target, {0, 1, 0});
        // Fit near/far to the scene so depth precision stays usable (a fixed
        // 0.02..1e5 range causes z-fighting flicker/stripes on large maps).
        // far just past the far side of the bounding sphere; near a small
        // fraction of the view distance, floored to keep far/near modest.
        float farP = g_dist + g_radius * 2.0f;
        float nearP = std::max(farP * 1e-4f, g_dist * 0.02f);
        if (nearP < 1e-3f) nearP = 1e-3f;
        tmath::Mat4 proj = tmath::perspectiveFovLH(45.0f * 3.14159265f / 180.0f,
                                             (float)g_w / std::max(1, g_h), nearP, farP);
        CB cb;
        cb.viewProj = tmath::mul(view, proj);   // clip = v * view * proj
        cb.lightDir[0] = -0.4f; cb.lightDir[1] = -0.7f; cb.lightDir[2] = -0.55f;
        cb.useTex = g_useTex ? 1.0f : 0.0f;  // untextured groups bind a 1x1 white
        cb.wireMode = 0.0f;
        cb.alphaTestRef = -1.0f;
        cb.lightOff = 0.0f;
        cb.fogEnable = g_fogEnable ? 1.0f : 0.0f;
        cb.fogColor[0] = g_fogColor[0]; cb.fogColor[1] = g_fogColor[1]; cb.fogColor[2] = g_fogColor[2];
        cb.fogStart = g_fogStart; cb.fogEnd = g_fogEnd;
        cb.camPos[0] = eye.x; cb.camPos[1] = eye.y; cb.camPos[2] = eye.z;
        // Shift fog to begin at the near side of the framed scene, so the orbit
        // camera (far from the whole map) doesn't fog everything uniformly.
        cb.fogBias = std::max(0.0f, g_dist - g_radius);

        auto uploadCB = [&]() {
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof(cb));
                g_ctx->Unmap(g_cb.Get(), 0);
            }
        };
        uploadCB();

        UINT stride = sizeof(Vertex), off = 0;
        ID3D11Buffer* vbs[] = {g_vb.Get()};
        g_ctx->IASetInputLayout(g_layout.Get());
        g_ctx->IASetVertexBuffers(0, 1, vbs, &stride, &off);
        g_ctx->IASetIndexBuffer(g_ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
        ID3D11Buffer* cbs[] = {g_cb.Get()};
        g_ctx->VSSetConstantBuffers(0, 1, cbs);
        g_ctx->PSSetConstantBuffers(0, 1, cbs);
        ID3D11ShaderResourceView* boneSrv[] = {g_boneSRV.Get()};
        g_ctx->VSSetShaderResources(1, 1, boneSrv);
        g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
        ID3D11SamplerState* smp[] = {g_sampler.Get()};
        g_ctx->PSSetSamplers(0, 1, smp);

        const float blendFactor[4] = {0, 0, 0, 0};
        // Draw one group with its own material state (blend/depth/alpha-test/light).
        auto drawGroup = [&](const DrawGroup& gp) {
            g_ctx->OMSetBlendState(gp.st.alphaBlend ? g_blendAlpha.Get() : g_blendOpaque.Get(),
                                   blendFactor, 0xffffffff);
            g_ctx->OMSetDepthStencilState((gp.st.alphaBlend || gp.st.zwriteOff)
                                              ? g_dssReadOnly.Get() : g_dss.Get(), 0);
            cb.alphaTestRef = gp.st.alphaTest ? gp.st.alphaRef
                                              : (gp.st.alphaBlend ? 0.004f : -1.0f);
            cb.lightOff = gp.st.lightOff ? 1.0f : 0.0f;
            uploadCB();
            ID3D11ShaderResourceView* srv = gp.srv ? gp.srv.Get() : g_white.Get();
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->DrawIndexed(gp.count, gp.offset, 0);
        };

        // Solid shaded pass: opaque/alpha-tested groups first (depth write), then
        // alpha-blended groups (depth-tested, no depth write), so transparency
        // composites over the scene without brown opaque fills or z-fighting.
        if (g_showMesh) {
            g_ctx->RSSetState(g_rasterSolid.Get());
            if (g_groups.empty()) {
                g_ctx->OMSetBlendState(g_blendOpaque.Get(), blendFactor, 0xffffffff);
                g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
                ID3D11ShaderResourceView* srv = g_white.Get();
                g_ctx->PSSetShaderResources(0, 1, &srv);
                g_ctx->DrawIndexed(g_indexCount, 0, 0);
            } else {
                for (const auto& gp : g_groups) if (!gp.st.alphaBlend) drawGroup(gp);
                for (const auto& gp : g_groups) if (gp.st.alphaBlend)  drawGroup(gp);
            }
            // Restore defaults for later passes.
            cb.alphaTestRef = -1.0f; cb.lightOff = 0.0f;
            g_ctx->OMSetBlendState(g_blendOpaque.Get(), blendFactor, 0xffffffff);
            g_ctx->OMSetDepthStencilState(g_dss.Get(), 0);
        }
        // Wireframe overlay pass (flat colour, biased above the solid mesh).
        if (g_wire) {
            cb.wireMode = 1.0f; uploadCB();
            g_ctx->RSSetState(g_rasterWire.Get());
            for (const auto& gp : g_groups) {
                ID3D11ShaderResourceView* srv = gp.srv ? gp.srv.Get() : g_white.Get();
                g_ctx->PSSetShaderResources(0, 1, &srv);
                g_ctx->DrawIndexed(gp.count, gp.offset, 0);
            }
            if (g_groups.empty()) g_ctx->DrawIndexed(g_indexCount, 0, 0);
            cb.wireMode = 0.0f; uploadCB();
        }

        // Skeleton overlay pass (depth-disabled line list).
        if (g_showSkeleton && g_skelVertCount > 0 && g_skelVB && g_skelVS && g_skelLayout) {
            UINT sstride = 3 * sizeof(float), soff = 0;
            ID3D11Buffer* svbs[] = {g_skelVB.Get()};
            g_ctx->IASetInputLayout(g_skelLayout.Get());
            g_ctx->IASetVertexBuffers(0, 1, svbs, &sstride, &soff);
            g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
            g_ctx->VSSetShader(g_skelVS.Get(), nullptr, 0);
            g_ctx->PSSetShader(g_skelPS.Get(), nullptr, 0);
            g_ctx->VSSetConstantBuffers(0, 1, cbs);
            g_ctx->OMSetDepthStencilState(g_dssNoDepth.Get(), 0);
            g_ctx->RSSetState(g_rasterSolid.Get());
            g_ctx->Draw((UINT)g_skelVertCount, 0);
        }

        // Particle billboard pass (map .3deffect preview). Quads are expanded on
        // the CPU to face the orbit camera; drawn depth-tested but not writing
        // depth, batched by texture + blend mode. A gentle time pulse gives the
        // static sprites a bit of life without a full simulation.
        if (g_showParticles && !g_billboards.empty() && g_bbVS && g_bbPS && g_bbLayout &&
            !g_particleTex.empty()) {
            tmath::Vec3 fwd = tmath::normalize(g_target - eye);
            tmath::Vec3 right = tmath::normalize(tmath::cross({0, 1, 0}, fwd));
            tmath::Vec3 up = tmath::cross(fwd, right);
            float tsec = GetTickCount64() * 0.001f;

            // Draw order: additive glows last (over alpha), each group by texture.
            std::vector<const Billboard*> order;
            order.reserve(g_billboards.size());
            for (const auto& b : g_billboards) order.push_back(&b);
            std::sort(order.begin(), order.end(), [](const Billboard* a, const Billboard* b) {
                if (a->additive != b->additive) return !a->additive;   // alpha first
                return a->texId < b->texId;
            });

            std::vector<BBVert> verts;
            verts.reserve(order.size() * 6);
            for (const Billboard* b : order) {
                float phase = b->pos[0] * 0.7f + b->pos[2] * 0.3f;
                float pulse = 0.82f + 0.18f * std::sin(tsec * 2.2f + phase);
                float s = b->size;
                tmath::Vec3 c{b->pos[0], b->pos[1], b->pos[2]};
                tmath::Vec3 R{right.x * s, right.y * s, right.z * s};
                tmath::Vec3 U{up.x * s, up.y * s, up.z * s};
                auto mk = [&](float sx, float sy, float u, float v) {
                    BBVert vx;
                    vx.px = c.x + R.x * sx + U.x * sy;
                    vx.py = c.y + R.y * sx + U.y * sy;
                    vx.pz = c.z + R.z * sx + U.z * sy;
                    vx.u = u; vx.v = v;
                    vx.r = b->col[0]; vx.g = b->col[1]; vx.b = b->col[2];
                    vx.a = b->col[3] * pulse;
                    return vx;
                };
                BBVert tl = mk(-1, 1, 0, 0), tr = mk(1, 1, 1, 0),
                       br = mk(1, -1, 1, 1), bl = mk(-1, -1, 0, 1);
                verts.push_back(tl); verts.push_back(tr); verts.push_back(br);
                verts.push_back(tl); verts.push_back(br); verts.push_back(bl);
            }

            // (Re)upload the dynamic vertex buffer.
            if ((int)verts.size() > g_bbVBCap) {
                g_bbVB.Reset();
                int cap = (int)verts.size() + 256;
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth = (UINT)(cap * sizeof(BBVert));
                bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                if (SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_bbVB))) g_bbVBCap = cap;
            }
            D3D11_MAPPED_SUBRESOURCE ms;
            if (g_bbVB && SUCCEEDED(g_ctx->Map(g_bbVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, verts.data(), verts.size() * sizeof(BBVert));
                g_ctx->Unmap(g_bbVB.Get(), 0);

                UINT bstride = sizeof(BBVert), boff = 0;
                ID3D11Buffer* bvbs[] = {g_bbVB.Get()};
                g_ctx->IASetInputLayout(g_bbLayout.Get());
                g_ctx->IASetVertexBuffers(0, 1, bvbs, &bstride, &boff);
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                g_ctx->VSSetShader(g_bbVS.Get(), nullptr, 0);
                g_ctx->PSSetShader(g_bbPS.Get(), nullptr, 0);
                g_ctx->VSSetConstantBuffers(0, 1, cbs);
                g_ctx->OMSetDepthStencilState(g_dssReadOnly.Get(), 0);
                g_ctx->RSSetState(g_rasterSolid.Get());
                ID3D11SamplerState* bsmp[] = {g_sampler.Get()};
                g_ctx->PSSetSamplers(0, 1, bsmp);

                // Draw contiguous runs sharing texture + blend mode.
                size_t i = 0;
                while (i < order.size()) {
                    int tex = order[i]->texId; bool add = order[i]->additive;
                    size_t j = i;
                    while (j < order.size() && order[j]->texId == tex && order[j]->additive == add) ++j;
                    g_ctx->OMSetBlendState(add ? g_blendAdd.Get() : g_blendAlpha.Get(),
                                           blendFactor, 0xffffffff);
                    ID3D11ShaderResourceView* srv = g_particleTex[tex].Get();
                    g_ctx->PSSetShaderResources(0, 1, &srv);
                    g_ctx->Draw((UINT)((j - i) * 6), (UINT)(i * 6));
                    i = j;
                }
                g_ctx->OMSetBlendState(g_blendOpaque.Get(), blendFactor, 0xffffffff);
            }
        }
    }
    g_swap->Present(1, 0);
}

} // namespace modelgfx
