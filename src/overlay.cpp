// D3D11-Overlay (siehe overlay.h). Ablauf: Schrift (3 Seiten 512x512 BGRA + Zeichentabelle) beim Start aus dem kpf lesen.
// Gezeichnet wird aus der Spielfunktion vor dem Present-Aufruf (RenderFrame, aus dllmain) - keine Aenderung an der dxgi-
// vtable: die teilen sich Steam-Overlay und andere Mods, gegenseitiges Umbiegen ergab eine Endlosschleife (Stack Overflow
// beim Start ueber Steam). Beim ersten Bild Geraet, Shader (D3DCompile aus d3dcompiler_47.dll), Texturen und Puffer anlegen;
// je Bild den Zustand des Spiels sichern, zeichnen, wiederherstellen. Die Zeichenflaeche (RTV) wird je Bild neu angelegt und
// freigegeben, damit kein Verweis auf den Hintergrundpuffer ein ResizeBuffers des Spiels blockiert.
#define _CRT_SECURE_NO_WARNINGS
#include "overlay.h"

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "coop_logic.h"
#include "i18n.h"

#pragma comment(lib, "d3d11.lib")

namespace overlay {
namespace {

LogFn g_log = nullptr;
FrameFn g_frameFn = nullptr;
coop::Glyph g_glyphs[256];
std::vector<uint8_t> g_pages[3];  // BGRA, oben zuerst

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11ShaderResourceView* g_srv[3] = {};
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_vb = nullptr;
ID3D11BlendState* g_blend = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
bool g_failed = false;
constexpr UINT kMaxVerts = 6 * 4096;

struct Vtx {
    float x, y, u, v;
    uint32_t col;  // RGBA8
};

const char* kShader = R"(
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 col : COLOR0; };
PSIn vs(VSIn i) { PSIn o; o.pos = float4(i.pos, 0, 1); o.uv = i.uv; o.col = i.col; return o; }
Texture2D tex : register(t0);
SamplerState smp : register(s0);
float4 ps(PSIn i) : SV_Target {
    if (i.uv.x < -0.5) return i.col;
    float4 t = tex.Sample(smp, i.uv);
    return float4(saturate(t.rgb * i.col.rgb * 1.5), t.a * i.col.a);
}
)";

template <typename T>
void SafeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

uint32_t Rgba(uint32_t rgb, float alpha) {
    const uint32_t a = static_cast<uint32_t>(coop::Clamp01(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return ((rgb >> 16) & 0xFF) | (((rgb >> 8) & 0xFF) << 8) | ((rgb & 0xFF) << 16) | (a << 24);
}

// TGA (unkomprimiert, 32 Bit) -> BGRA oben zuerst
bool DecodeTga(const std::vector<uint8_t>& tga, std::vector<uint8_t>& out) {
    if (tga.size() < 18 || tga[2] != 2 || tga[16] != 32) return false;
    const int w = tga[12] | (tga[13] << 8), h = tga[14] | (tga[15] << 8);
    const size_t off = 18 + tga[0];
    if (w != 512 || h != 512 || tga.size() < off + static_cast<size_t>(w) * h * 4) return false;
    out.resize(static_cast<size_t>(w) * h * 4);
    const bool topDown = (tga[17] & 0x20) != 0;
    for (int y = 0; y < h; ++y)
        std::memcpy(&out[static_cast<size_t>(y) * w * 4], &tga[off + static_cast<size_t>(topDown ? y : h - 1 - y) * w * 4], w * 4);
    return true;
}

bool LoadFont(const char* kpf) {
    const std::vector<uint8_t> table = coop::KpfReadFile(kpf, "fonts/FE/ftable.dat");
    if (!coop::ParseFtable(table.data(), table.size(), g_glyphs)) return false;
    const char* names[3] = {"fonts/FE/000NOMIP.tga", "fonts/FE/001NOMIP.tga", "fonts/FE/002NOMIP.tga"};
    for (int i = 0; i < 3; ++i)
        if (!DecodeTga(coop::KpfReadFile(kpf, names[i]), g_pages[i])) return false;
    return true;
}

bool CreateResources(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) return false;
    g_dev->GetImmediateContext(&g_ctx);
    HMODULE comp = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = comp ? reinterpret_cast<pD3DCompile>(GetProcAddress(comp, "D3DCompile")) : nullptr;
    if (!compile) {
        g_log(L("[Namen] d3dcompiler_47.dll fehlt\n", "[Names] d3dcompiler_47.dll missing\n"));
        return false;
    }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, std::strlen(kShader), "overlay", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kShader, std::strlen(kShader), "overlay", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        g_log(L("[Namen] Shader-Fehler: %s\n", "[Names] shader error: %s\n"), err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        SafeRelease(err);
        SafeRelease(vsb);
        return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    g_dev->CreateInputLayout(layout, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
    SafeRelease(vsb);
    SafeRelease(psb);
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = kMaxVerts * sizeof(Vtx);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_vb);
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_dev->CreateBlendState(&bl, &g_blend);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&rd, &g_raster);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dev->CreateDepthStencilState(&dd, &g_depth);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_sampler);
    for (int i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 512;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init = {g_pages[i].data(), 512 * 4, 0};
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&td, &init, &tex)) && tex) {
            g_dev->CreateShaderResourceView(tex, nullptr, &g_srv[i]);
            tex->Release();
        }
    }
    const bool ok = g_vs && g_ps && g_layout && g_vb && g_blend && g_raster && g_depth && g_sampler && g_srv[0] && g_srv[1] && g_srv[2];
    g_log(L("[Namen] Overlay %s\n", "[Names] overlay %s\n"), ok ? L("bereit", "ready") : L("FEHLER beim Anlegen", "ERROR creating"));
    return ok;
}

// neues Geraet des Spiels (z.B. Vollbild umgeschaltet): alles freigeben, beim naechsten Bild neu anlegen
void ReleaseResources() {
    for (auto*& v : g_srv) SafeRelease(v);
    SafeRelease(g_vs);
    SafeRelease(g_ps);
    SafeRelease(g_layout);
    SafeRelease(g_vb);
    SafeRelease(g_blend);
    SafeRelease(g_raster);
    SafeRelease(g_depth);
    SafeRelease(g_sampler);
    SafeRelease(g_ctx);
    SafeRelease(g_dev);
}

// ---- Geometrie sammeln: je Schriftseite und fuer Flaechen (Pfeile) ----
struct Batch {
    std::vector<Vtx> page[3];
    std::vector<Vtx> solid;
};

void Quad(std::vector<Vtx>& out, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t col,
          float sw, float sh) {
    auto nx = [sw](float px) { return px / sw * 2.0f - 1.0f; };
    auto ny = [sh](float py) { return 1.0f - py / sh * 2.0f; };
    const Vtx a{nx(x0), ny(y0), u0, v0, col}, b{nx(x1), ny(y0), u1, v0, col}, c{nx(x0), ny(y1), u0, v1, col},
        d{nx(x1), ny(y1), u1, v1, col};
    out.insert(out.end(), {a, b, c, b, d, c});
}

float TextWidth(const char* text, float s) {
    float w = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
        const int k = *p - 33;
        if (k >= 0 && k < 256 && g_glyphs[k].valid) w += (g_glyphs[k].w + 3.0f) * s;
        else w += 18.0f * s;
    }
    return w;
}

void AddText(Batch& b, const Label& l, float sw, float sh) {
    const float s = sh * 0.042f / 63.0f * l.scale;  // Pixel je Schrift-Pixel
    float x = (l.x * 0.5f + 0.5f) * sw, y = (0.5f - l.y * 0.5f) * sh;
    if (l.align == 0) x -= TextWidth(l.text, s) * 0.5f;
    const uint32_t shadow = Rgba(0x000000, l.alpha * 0.85f), col = Rgba(l.rgb, l.alpha);
    for (int pass = 0; pass < 2; ++pass) {
        float pen = x + (pass ? 0.0f : 2.0f * s * 1.5f);
        const float top = y + (pass ? 0.0f : 2.0f * s * 1.5f);
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(l.text); *p; ++p) {
            const int k = *p - 33;
            if (k < 0 || k >= 256 || !g_glyphs[k].valid) {
                pen += 18.0f * s;
                continue;
            }
            const coop::Glyph& g = g_glyphs[k];
            Quad(b.page[g.page], pen, top, pen + g.w * s, top + g.h * s, g.u0, g.v0, g.u1, g.v1, pass ? col : shadow, sw, sh);
            pen += (g.w + 3.0f) * s;
        }
    }
}

// Pfeilspitze mit Widerhaken: Spitze, zwei Haken, Kerbe hinten; zuerst dunkler Rand, dann Farbe
void AddArrow(Batch& b, const Arrow& a, float sw, float sh) {
    const float len = sh * 0.026f * a.size;
    const float tipX = (a.x * 0.5f + 0.5f) * sw, tipY = (0.5f - a.y * 0.5f) * sh;
    const float c = std::cos(a.angle), sn = -std::sin(a.angle);  // Bildschirm-y zeigt nach unten
    auto pt = [&](float fx, float fy, float grow, float& px, float& py) {
        fx *= grow;
        fy *= grow;
        px = tipX + (fx - len * (grow - 1.0f) * 0.4f) * c - fy * sn;
        py = tipY + (fx - len * (grow - 1.0f) * 0.4f) * sn + fy * c;
    };
    auto nx = [sw](float px) { return px / sw * 2.0f - 1.0f; };
    auto ny = [sh](float py) { return 1.0f - py / sh * 2.0f; };
    for (int pass = 0; pass < 2; ++pass) {
        const float grow = pass ? 1.0f : 1.3f;
        const uint32_t col = pass ? Rgba(a.rgb, a.alpha) : Rgba(0x120A08, a.alpha * 0.9f);
        float tx, ty, lx, ly, rx, ry, kx, ky;
        pt(0.0f, 0.0f, grow, tx, ty);                // Spitze
        pt(-len, len * 0.55f, grow, lx, ly);         // Haken links
        pt(-len * 0.55f, 0.0f, grow, kx, ky);        // Kerbe
        pt(-len, -len * 0.55f, grow, rx, ry);        // Haken rechts
        const Vtx t{nx(tx), ny(ty), -1, -1, col}, l{nx(lx), ny(ly), -1, -1, col}, k{nx(kx), ny(ky), -1, -1, col},
            r{nx(rx), ny(ry), -1, -1, col};
        b.solid.insert(b.solid.end(), {t, l, k, t, k, r});
    }
}

struct SavedState {
    ID3D11RenderTargetView* rtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    D3D11_VIEWPORT vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT numVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11BlendState* blend = nullptr;
    float blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencilRef = 0;
    ID3D11RasterizerState* raster = nullptr;
    ID3D11InputLayout* layout = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
    UINT ibOffset = 0;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11SamplerState* sampler = nullptr;

    void Save(ID3D11DeviceContext* c) {
        c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, &dsv);
        c->RSGetViewports(&numVp, vp);
        c->OMGetBlendState(&blend, blendFactor, &sampleMask);
        c->OMGetDepthStencilState(&depth, &stencilRef);
        c->RSGetState(&raster);
        c->IAGetInputLayout(&layout);
        c->IAGetPrimitiveTopology(&topo);
        c->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IAGetIndexBuffer(&ib, &ibFormat, &ibOffset);
        c->VSGetShader(&vs, nullptr, nullptr);
        c->PSGetShader(&ps, nullptr, nullptr);
        c->GSGetShader(&gs, nullptr, nullptr);
        c->HSGetShader(&hs, nullptr, nullptr);
        c->DSGetShader(&ds, nullptr, nullptr);
        c->PSGetShaderResources(0, 1, &srv);
        c->PSGetSamplers(0, 1, &sampler);
    }
    void Restore(ID3D11DeviceContext* c) {
        c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtv, dsv);
        c->RSSetViewports(numVp, vp);
        c->OMSetBlendState(blend, blendFactor, sampleMask);
        c->OMSetDepthStencilState(depth, stencilRef);
        c->RSSetState(raster);
        c->IASetInputLayout(layout);
        c->IASetPrimitiveTopology(topo);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        c->IASetIndexBuffer(ib, ibFormat, ibOffset);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetShader(ps, nullptr, 0);
        c->GSSetShader(gs, nullptr, 0);
        c->HSSetShader(hs, nullptr, 0);
        c->DSSetShader(ds, nullptr, 0);
        c->PSSetShaderResources(0, 1, &srv);
        c->PSSetSamplers(0, 1, &sampler);
        for (auto*& r : rtv) SafeRelease(r);
        SafeRelease(dsv);
        SafeRelease(blend);
        SafeRelease(depth);
        SafeRelease(raster);
        SafeRelease(layout);
        SafeRelease(vb);
        SafeRelease(ib);
        SafeRelease(vs);
        SafeRelease(ps);
        SafeRelease(gs);
        SafeRelease(hs);
        SafeRelease(ds);
        SafeRelease(srv);
        SafeRelease(sampler);
    }
};

void DrawList(const std::vector<Vtx>& v, ID3D11ShaderResourceView* srv) {
    if (v.empty()) return;
    for (size_t first = 0; first < v.size(); first += kMaxVerts) {
        const UINT n = static_cast<UINT>(v.size() - first < kMaxVerts ? v.size() - first : kMaxVerts);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(g_ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
        std::memcpy(m.pData, &v[first], n * sizeof(Vtx));
        g_ctx->Unmap(g_vb, 0);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        g_ctx->Draw(n, 0);
    }
}

void Render(IDXGISwapChain* sc) {
    if (g_failed || !g_frameFn) return;
    if (g_dev) {
        ID3D11Device* current = nullptr;
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&current))) && current) current->Release();
        if (current != g_dev) {
            g_log(L("[Namen] neues Grafikgeraet - Overlay wird neu angelegt\n", "[Names] new graphics device - recreating overlay\n"));
            ReleaseResources();
        }
    }
    if (!g_dev && !CreateResources(sc)) {
        g_failed = true;
        return;
    }
    DXGI_SWAP_CHAIN_DESC scd;
    if (FAILED(sc->GetDesc(&scd))) return;
    const float sw = static_cast<float>(scd.BufferDesc.Width), sh = static_cast<float>(scd.BufferDesc.Height);
    static Frame frame;
    frame.numLabels = 0;
    frame.numArrows = 0;
    g_frameFn(frame, sw, sh);
    if (!frame.numLabels && !frame.numArrows) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) return;
    ID3D11RenderTargetView* rtv = nullptr;
    const HRESULT hr = g_dev->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr) || !rtv) return;
    static Batch batch;
    for (auto& p : batch.page) p.clear();
    batch.solid.clear();
    for (int i = 0; i < frame.numArrows; ++i) AddArrow(batch, frame.arrows[i], sw, sh);
    for (int i = 0; i < frame.numLabels; ++i) AddText(batch, frame.labels[i], sw, sh);
    SavedState saved;
    saved.Save(g_ctx);
    D3D11_VIEWPORT vp = {0, 0, sw, sh, 0, 1};
    g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx->RSSetViewports(1, &vp);
    const float bf[4] = {};
    g_ctx->OMSetBlendState(g_blend, bf, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(g_depth, 0);
    g_ctx->RSSetState(g_raster);
    g_ctx->IASetInputLayout(g_layout);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(Vtx), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->HSSetShader(nullptr, nullptr, 0);
    g_ctx->DSSetShader(nullptr, nullptr, 0);
    g_ctx->PSSetSamplers(0, 1, &g_sampler);
    DrawList(batch.solid, g_srv[0]);
    for (int i = 0; i < 3; ++i) DrawList(batch.page[i], g_srv[i]);
    saved.Restore(g_ctx);
    rtv->Release();
}

void RenderGuarded(IDXGISwapChain* sc) {
    __try {
        Render(sc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (!g_failed) g_log(L("[Namen] Ausnahme im Overlay - abgeschaltet\n", "[Names] exception in overlay - disabled\n"));
        g_failed = true;
    }
}

}  // namespace

bool Install(const char* kpfPath, FrameFn frameFn, LogFn log) {
    g_log = log;
    g_frameFn = frameFn;
    if (!LoadFont(kpfPath)) {
        log(L("[Namen] Schrift FE nicht gefunden (%s)\n", "[Names] font FE not found (%s)\n"), kpfPath);
        return false;
    }
    log(L("[Namen] Schrift FE geladen\n", "[Names] font FE loaded\n"));
    return true;
}

void RenderFrame(void* swapChain) {
    if (swapChain) RenderGuarded(static_cast<IDXGISwapChain*>(swapChain));
}

}  // namespace overlay
