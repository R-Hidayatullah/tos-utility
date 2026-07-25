#include "model3d.h"
#include "client_gfx.h"
#include "tinymath.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cstring>
#include <vector>

namespace m3d {
namespace {

using Microsoft::WRL::ComPtr;

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<ID3D11InputLayout> g_layout;
ComPtr<ID3D11Buffer> g_cb;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11ShaderResourceView> g_white;
ComPtr<ID3D11DepthStencilState> g_dssOpaque, g_dssTrans;
ComPtr<ID3D11BlendState> g_blendOpaque, g_blendAlpha;
ComPtr<ID3D11RasterizerState> g_raster;

tmath::Mat4 g_camVP = tmath::identity();
float g_camPos[3] = {0, 0, 0};
float g_lightDir[3] = {-0.4f, -0.7f, -0.55f};

struct CB {
    tmath::Mat4 viewProj;
    float lightDir[3]; float useTex;
    float alphaTestRef; float lightOff; float _pad[2];
};

constexpr char kShader[] = R"(
cbuffer CB : register(b0) {
    row_major float4x4 viewProj;
    float3 lightDir; float useTex;
    float alphaTestRef; float lightOff; float2 _pad;
};
struct Bone { row_major float4x4 m; };
StructuredBuffer<Bone> gBones : register(t1);
struct VSIn  { float3 pos:POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD;
               float4 bone:BLENDINDICES; float4 weight:BLENDWEIGHT; };
struct VSOut { float4 pos:SV_POSITION; float3 nrm:NORMAL; float2 uv:TEXCOORD; };
VSOut VSMain(VSIn i){
    float4x4 skin = gBones[(uint)i.bone.x].m * i.weight.x
                  + gBones[(uint)i.bone.y].m * i.weight.y
                  + gBones[(uint)i.bone.z].m * i.weight.z
                  + gBones[(uint)i.bone.w].m * i.weight.w;
    float3 sp = mul(float4(i.pos,1.0), skin).xyz;
    float3 sn = mul(float4(i.nrm,0.0), skin).xyz;
    VSOut o; o.pos = mul(float4(sp,1.0), viewProj); o.nrm = sn; o.uv = i.uv; return o;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(VSOut i):SV_TARGET{
    float4 t = tex0.Sample(samp0, i.uv);
    float a = (useTex > 0.5) ? t.a : 1.0;
    if (alphaTestRef >= 0.0) clip(a - alphaTestRef);
    float3 base = (useTex > 0.5) ? t.rgb : float3(0.75,0.76,0.80);
    float3 col;
    if (lightOff > 0.5 && useTex > 0.5) col = base;
    else { float ndl = saturate(dot(normalize(i.nrm), -normalize(lightDir)));
           col = base * (0.32 + 0.68*ndl); }
    return float4(col, a);
}
)";

struct DrawGroup {
    UINT off = 0, count = 0;
    ComPtr<ID3D11ShaderResourceView> srv;
    GroupState st;
};

ComPtr<ID3D11ShaderResourceView> make_srv(uint32_t fmt, uint32_t w, uint32_t h,
                                          uint32_t pitch, const uint8_t* data) {
    ComPtr<ID3D11ShaderResourceView> srv;
    if (!data || !w || !h) return srv;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)fmt; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = data; sd.SysMemPitch = pitch;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, tex.GetAddressOf()))) return srv;
    g_dev->CreateShaderResourceView(tex.Get(), nullptr, srv.GetAddressOf());
    return srv;
}

} // namespace

struct Model {
    ComPtr<ID3D11Buffer> vb, ib;
    UINT indexCount = 0;
    std::vector<DrawGroup> groups;
    ComPtr<ID3D11Buffer> boneBuf;
    ComPtr<ID3D11ShaderResourceView> boneSRV;
    int boneCap = 0, boneCount = 0;
};

bool init() {
    g_dev = cgfx::device();
    g_ctx = cgfx::context();
    if (!g_dev) return false;

    ComPtr<ID3DBlob> vsb, psb, err;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "m3d", nullptr, nullptr,
            "VSMain", "vs_4_0", 0, 0, vsb.GetAddressOf(), err.GetAddressOf()))) return false;
    if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "m3d", nullptr, nullptr,
            "PSMain", "ps_4_0", 0, 0, psb.GetAddressOf(), err.ReleaseAndGetAddressOf()))) return false;
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, g_vs.GetAddressOf());
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, g_ps.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 48, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_dev->CreateInputLayout(il, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), g_layout.GetAddressOf());

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CB); cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&cbd, nullptr, g_cb.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC o{};
    o.DepthEnable = TRUE; o.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; o.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&o, g_dssOpaque.GetAddressOf());
    D3D11_DEPTH_STENCIL_DESC tr{};
    tr.DepthEnable = TRUE; tr.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO; tr.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    g_dev->CreateDepthStencilState(&tr, g_dssTrans.GetAddressOf());

    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bo, g_blendOpaque.GetAddressOf());
    D3D11_BLEND_DESC ba{};
    ba.RenderTarget[0].BlendEnable = TRUE;
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&ba, g_blendAlpha.GetAddressOf());

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, g_raster.GetAddressOf());

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC; sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, g_sampler.GetAddressOf());

    uint8_t wpx[4] = {255, 255, 255, 255};
    g_white = make_srv(DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 4, wpx);
    return true;
}

void shutdown() {}

Model* create() { return new Model(); }
void destroy(Model* m) { delete m; }

void set_geometry(Model* m, const modelgfx::Vertex* v, size_t vc,
                  const uint32_t* idx, size_t ic, int boneCount) {
    m->vb.Reset(); m->ib.Reset();
    if (vc) {
        D3D11_BUFFER_DESC vd{}; vd.ByteWidth = (UINT)(vc * sizeof(modelgfx::Vertex));
        vd.Usage = D3D11_USAGE_IMMUTABLE; vd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{v, 0, 0};
        g_dev->CreateBuffer(&vd, &sd, m->vb.GetAddressOf());
    }
    if (ic) {
        D3D11_BUFFER_DESC id{}; id.ByteWidth = (UINT)(ic * sizeof(uint32_t));
        id.Usage = D3D11_USAGE_IMMUTABLE; id.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA sd{idx, 0, 0};
        g_dev->CreateBuffer(&id, &sd, m->ib.GetAddressOf());
    }
    m->indexCount = (UINT)ic;

    // Bone structured buffer.
    if (boneCount < 1) boneCount = 1;
    if (boneCount != m->boneCap) {
        m->boneBuf.Reset(); m->boneSRV.Reset();
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = boneCount * 64; bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; bd.StructureByteStride = 64;
        g_dev->CreateBuffer(&bd, nullptr, m->boneBuf.GetAddressOf());
        D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        sv.Format = DXGI_FORMAT_UNKNOWN;
        sv.Buffer.NumElements = boneCount;
        g_dev->CreateShaderResourceView(m->boneBuf.Get(), &sv, m->boneSRV.GetAddressOf());
        m->boneCap = boneCount;
    }
    m->boneCount = boneCount;
    // default bind pose = identity matrices
    std::vector<float> ident(boneCount * 16, 0.f);
    for (int i = 0; i < boneCount; ++i) { float* p = &ident[i * 16]; p[0] = p[5] = p[10] = p[15] = 1.f; }
    upload_bones(m, ident.data(), boneCount);
}

void clear_groups(Model* m) { m->groups.clear(); }

void add_group(Model* m, uint32_t off, uint32_t count, uint32_t fmt, uint32_t w, uint32_t h,
               uint32_t pitch, const uint8_t* data, uint32_t, GroupState st) {
    DrawGroup g; g.off = off; g.count = count; g.st = st;
    g.srv = make_srv(fmt, w, h, pitch, data);
    m->groups.push_back(std::move(g));
}

void upload_bones(Model* m, const float* mats, int count) {
    if (!m->boneBuf || count < 1) return;
    if (count > m->boneCap) count = m->boneCap;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(m->boneBuf.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, mats, count * 64);
        g_ctx->Unmap(m->boneBuf.Get(), 0);
    }
}

void set_camera(const float* vp, const float* camPos, const float* lightDir) {
    std::memcpy(&g_camVP.m[0][0], vp, 16 * sizeof(float));
    if (camPos) std::memcpy(g_camPos, camPos, 3 * sizeof(float));
    if (lightDir) std::memcpy(g_lightDir, lightDir, 3 * sizeof(float));
}

void draw(Model* m, const float* world16) {
    if (!m || !m->vb || !m->ib || !m->boneSRV) return;

    tmath::Mat4 world;
    std::memcpy(&world.m[0][0], world16, 16 * sizeof(float));
    tmath::Mat4 finalVP = tmath::mul(world, g_camVP);

    g_ctx->IASetInputLayout(g_layout.Get());
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(modelgfx::Vertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, m->vb.GetAddressOf(), &stride, &offset);
    g_ctx->IASetIndexBuffer(m->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
    g_ctx->VSSetShader(g_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_ps.Get(), nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
    g_ctx->PSSetConstantBuffers(0, 1, g_cb.GetAddressOf());
    g_ctx->VSSetShaderResources(1, 1, m->boneSRV.GetAddressOf());
    g_ctx->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
    g_ctx->RSSetState(g_raster.Get());

    // Two passes: opaque groups first (depth write), then blended (depth read-only).
    for (int pass = 0; pass < 2; ++pass) {
        bool wantBlend = (pass == 1);
        g_ctx->OMSetDepthStencilState(wantBlend ? g_dssTrans.Get() : g_dssOpaque.Get(), 0);
        float bf[4] = {0, 0, 0, 0};
        g_ctx->OMSetBlendState(wantBlend ? g_blendAlpha.Get() : g_blendOpaque.Get(), bf, 0xffffffff);
        for (const auto& g : m->groups) {
            if (g.st.alphaBlend != wantBlend) continue;
            CB cb;
            cb.viewProj = finalVP;
            cb.lightDir[0] = g_lightDir[0]; cb.lightDir[1] = g_lightDir[1]; cb.lightDir[2] = g_lightDir[2];
            cb.useTex = g.srv ? 1.f : 0.f;
            cb.alphaTestRef = g.st.alphaTest ? g.st.alphaRef : -1.f;
            cb.lightOff = g.st.lightOff ? 1.f : 0.f;
            cb._pad[0] = cb._pad[1] = 0;
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &cb, sizeof(cb));
                g_ctx->Unmap(g_cb.Get(), 0);
            }
            ID3D11ShaderResourceView* srv = g.srv ? g.srv.Get() : g_white.Get();
            g_ctx->PSSetShaderResources(0, 1, &srv);
            g_ctx->DrawIndexed(g.count, g.off, 0);
        }
    }
}

} // namespace m3d
