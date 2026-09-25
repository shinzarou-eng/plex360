// renderer.cpp — mini renderer D3D9 Xbox 360
// Vertex en clip-space calcule cote CPU, 3 batches : couleur / images / texte.
#include "renderer.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "font8x8.h"

#define SCREEN_W   1280
#define SCREEN_H   720
#define MAX_VERTS  (6 * 4096)

namespace Render {

struct Vertex {
    float x, y, z, w;
    DWORD color;
    float u, v;
};

struct ImgCmd { IDirect3DTexture9* tex; int startVert; int vertCount; };

static IDirect3DDevice9*           s_dev      = NULL;
static IDirect3DVertexShader9*     s_vs       = NULL;
static IDirect3DPixelShader9*      s_psColor  = NULL;
static IDirect3DPixelShader9*      s_psTex    = NULL;
static IDirect3DVertexDeclaration9* s_decl    = NULL;
static IDirect3DVertexBuffer9*     s_vb       = NULL;
static IDirect3DTexture9*          s_fontTex  = NULL;

static Vertex s_colorVerts[MAX_VERTS];
static int    s_nColor = 0;
static Vertex s_imgVerts[MAX_VERTS];
static int    s_nImg   = 0;
static Vertex s_textVerts[MAX_VERTS];
static int    s_nText  = 0;
static ImgCmd s_imgCmds[512];
static int    s_nImgCmds = 0;

static inline float ClipX(float px) { return (px / (float)SCREEN_W) * 2.0f - 1.0f; }
static inline float ClipY(float py) { return 1.0f - (py / (float)SCREEN_H) * 2.0f; }

static void SetV(Vertex* v, float px, float py, DWORD color, float u, float vv)
{
    v->x = ClipX(px); v->y = ClipY(py); v->z = 0.5f; v->w = 1.0f;
    v->color = color; v->u = u; v->v = vv;
}

static void PushQuad(Vertex* buf, int& n, float x, float y, float w, float h,
                     DWORD cTL, DWORD cTR, DWORD cBR, DWORD cBL,
                     float u0, float v0, float u1, float v1)
{
    if (n + 6 > MAX_VERTS) return;
    SetV(&buf[n+0], x,   y,   cTL, u0, v0);
    SetV(&buf[n+1], x+w, y,   cTR, u1, v0);
    SetV(&buf[n+2], x,   y+h, cBL, u0, v1);
    SetV(&buf[n+3], x+w, y,   cTR, u1, v0);
    SetV(&buf[n+4], x+w, y+h, cBR, u1, v1);
    SetV(&buf[n+5], x,   y+h, cBL, u0, v1);
    n += 6;
}

// ---------------------------------------------------------------------------
void FillRect(float x, float y, float w, float h, DWORD color)
{
    PushQuad(s_colorVerts, s_nColor, x, y, w, h, color, color, color, color, 0,0,0,0);
}

void FillRectGradV(float x, float y, float w, float h, DWORD cTop, DWORD cBottom)
{
    PushQuad(s_colorVerts, s_nColor, x, y, w, h, cTop, cTop, cBottom, cBottom, 0,0,0,0);
}

void RectOutline(float x, float y, float w, float h, float t, DWORD color)
{
    FillRect(x, y, w, t, color);
    FillRect(x, y + h - t, w, t, color);
    FillRect(x, y, t, h, color);
    FillRect(x + w - t, y, t, h, color);
}

void Image(IDirect3DTexture9* tex, float x, float y, float w, float h, DWORD tint)
{
    if (!tex || s_nImgCmds >= 512) return;
    ImgCmd& c = s_imgCmds[s_nImgCmds++];
    c.tex = tex; c.startVert = s_nImg; c.vertCount = 6;
    PushQuad(s_imgVerts, s_nImg, x, y, w, h, tint, tint, tint, tint, 0.f, 0.f, 1.f, 1.f);
}

// ---------------------------------------------------------------------------
// Texte
// ---------------------------------------------------------------------------
// Replie les accents Latin-1 vers ASCII (la font 8x8 n'a pas ces glyphes)
static char FoldLatin1(unsigned cp)
{
    if (cp >= 0xC0 && cp <= 0xC5) return 'A';
    if (cp == 0xC6) return 'A'; // AE
    if (cp == 0xC7) return 'C';
    if (cp >= 0xC8 && cp <= 0xCB) return 'E';
    if (cp >= 0xCC && cp <= 0xCF) return 'I';
    if (cp == 0xD0) return 'D';
    if (cp == 0xD1) return 'N';
    if (cp >= 0xD2 && cp <= 0xD8) return 'O';
    if (cp >= 0xD9 && cp <= 0xDC) return 'U';
    if (cp == 0xDD) return 'Y';
    if (cp >= 0xE0 && cp <= 0xE5) return 'a';
    if (cp == 0xE6) return 'a';
    if (cp == 0xE7) return 'c';
    if (cp >= 0xE8 && cp <= 0xEB) return 'e';
    if (cp >= 0xEC && cp <= 0xEF) return 'i';
    if (cp == 0xF1) return 'n';
    if (cp >= 0xF2 && cp <= 0xF8) return 'o';
    if (cp >= 0xF9 && cp <= 0xFC) return 'u';
    if (cp == 0xFD || cp == 0xFF) return 'y';
    if (cp == 0xDF) return 's'; // eszett
    return '?';
}

void Text(float x, float y, float scale, DWORD color, const char* str)
{
    float cx = x;
    for (const char* p = str; *p && s_nText + 6 <= MAX_VERTS; ++p) {
        unsigned char c = (unsigned char)*p;
        // UTF-8 2 octets (Latin-1) -> replie ; sequences plus longues -> '?'
        if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            unsigned cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
            c = (unsigned char)FoldLatin1(cp); ++p;
        } else if ((c & 0xF0) == 0xE0) {
            c = '?';
            if ((p[1] & 0xC0) == 0x80) ++p;
            if ((p[1] & 0xC0) == 0x80) ++p;
        } else if ((c & 0xF8) == 0xF0) {
            c = '?';
            for (int k = 0; k < 3 && (p[1] & 0xC0) == 0x80; ++k) ++p;
        }
        if (c < 32 || c > 126) c = '?';
        int idx  = c - 32;
        float tx = (float)((idx % 16) * 8);
        float ty = (float)((idx / 16) * 8);
        PushQuad(s_textVerts, s_nText, cx, y, 8.f * scale, 8.f * scale,
                 color, color, color, color,
                 tx / 128.f, ty / 48.f, (tx + 8.f) / 128.f, (ty + 8.f) / 48.f);
        cx += 8.f * scale;
    }
}

void TextF(float x, float y, float scale, DWORD color, const char* fmt, ...)
{
    char buf[1024];
    va_list args; va_start(args, fmt);
    vsprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);
    Text(x, y, scale, color, buf);
}

float TextWidth(const char* str, float scale)
{
    return (float)strlen(str) * 8.f * scale;
}

void TextEllipsis(float x, float y, float scale, DWORD color, const char* str, float maxWidthPx)
{
    int maxChars = (int)(maxWidthPx / (8.f * scale));
    int len = (int)strlen(str);
    if (len <= maxChars) { Text(x, y, scale, color, str); return; }
    char buf[256];
    int keep = maxChars - 3;
    if (keep < 0) keep = 0;
    if (keep > 250) keep = 250;
    memcpy(buf, str, keep); buf[keep] = 0;
    strcat_s(buf, sizeof(buf), "...");
    Text(x, y, scale, color, buf);
}

// ---------------------------------------------------------------------------
// Init / frame
// ---------------------------------------------------------------------------
static const char* kVS =
    "struct VI{float4 pos:POSITION;float4 col:COLOR0;float2 uv:TEXCOORD0;};"
    "struct VO{float4 pos:POSITION;float4 col:COLOR0;float2 uv:TEXCOORD0;};"
    "VO main(VI i){VO o;o.pos=i.pos;o.col=i.col;o.uv=i.uv;return o;}";
static const char* kPSColor =
    "float4 main(float4 col:COLOR0):COLOR{return col;}";
static const char* kPSTex =
    "sampler s0:register(s0);"
    "float4 main(float4 col:COLOR0,float2 uv:TEXCOORD0):COLOR{return tex2D(s0,uv)*col;}";

static HRESULT CompileShader(const char* src, const char* profile, void** out)
{
    ID3DXBuffer* code = NULL; ID3DXBuffer* err = NULL;
    HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main",
                                   profile, 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) { OutputDebugStringA((const char*)err->GetBufferPointer()); err->Release(); }
        return hr;
    }
    if (err) err->Release();
    if (profile[0] == 'v')
        hr = s_dev->CreateVertexShader((DWORD*)code->GetBufferPointer(), (IDirect3DVertexShader9**)out);
    else
        hr = s_dev->CreatePixelShader((DWORD*)code->GetBufferPointer(), (IDirect3DPixelShader9**)out);
    code->Release();
    return hr;
}

static HRESULT BuildFontTexture()
{
    // LIN_ = layout lineaire : LockRect expose les pixels en ordre raster
    // (le format tiled par defaut produirait des glyphes brouilles)
    HRESULT hr = s_dev->CreateTexture(128, 48, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                      D3DPOOL_MANAGED, &s_fontTex, NULL);
    if (FAILED(hr)) return hr;
    D3DLOCKED_RECT lr;
    if (FAILED(s_fontTex->LockRect(0, &lr, NULL, 0))) return E_FAIL;
    for (int c = 32; c < 127; ++c) {
        int cx = ((c - 32) % 16) * 8, cy = ((c - 32) / 16) * 8;
        for (int gy = 0; gy < 8; ++gy) {
            unsigned char bits = font8x8_basic[c][gy];
            DWORD* row = (DWORD*)((BYTE*)lr.pBits + (cy + gy) * lr.Pitch);
            for (int gx = 0; gx < 8; ++gx)
                row[cx + gx] = ((bits >> gx) & 1) ? 0xFFFFFFFF : 0x00FFFFFF;
        }
    }
    s_fontTex->UnlockRect(0);
    return S_OK;
}

HRESULT Init(IDirect3DDevice9* device)
{
    s_dev = device;
    s_dev->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);
    s_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    s_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    s_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    s_dev->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    s_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    s_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    s_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 20, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    if (FAILED(s_dev->CreateVertexDeclaration(decl, &s_decl))) return E_FAIL;
    if (FAILED(s_dev->CreateVertexBuffer(sizeof(Vertex) * MAX_VERTS * 3,
               D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s_vb, NULL)))
        return E_FAIL;
    if (FAILED(CompileShader(kVS,      "vs_3_0", (void**)&s_vs)))      return E_FAIL;
    if (FAILED(CompileShader(kPSColor, "ps_3_0", (void**)&s_psColor))) return E_FAIL;
    if (FAILED(CompileShader(kPSTex,   "ps_3_0", (void**)&s_psTex)))   return E_FAIL;
    if (FAILED(BuildFontTexture())) return E_FAIL;
    return S_OK;
}

void Shutdown()
{
    if (s_fontTex) s_fontTex->Release();
    if (s_vb)      s_vb->Release();
    if (s_decl)    s_decl->Release();
    if (s_vs)      s_vs->Release();
    if (s_psColor) s_psColor->Release();
    if (s_psTex)   s_psTex->Release();
}

void BeginFrame()
{
    s_nColor = 0; s_nImg = 0; s_nText = 0; s_nImgCmds = 0;
    s_dev->BeginScene();
    s_dev->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
}

void FlushBatches() { s_nColor = 0; s_nImg = 0; s_nText = 0; s_nImgCmds = 0; }

void EndFrame()
{
    int totalVerts = s_nColor + s_nImg + s_nText;
    if (totalVerts > 0) {
        void* p = NULL;
        if (SUCCEEDED(s_vb->Lock(0, 0, &p, 0))) {
            memcpy(p, s_colorVerts, s_nColor * sizeof(Vertex));
            memcpy((BYTE*)p + s_nColor * sizeof(Vertex), s_imgVerts, s_nImg * sizeof(Vertex));
            memcpy((BYTE*)p + (s_nColor + s_nImg) * sizeof(Vertex), s_textVerts, s_nText * sizeof(Vertex));
            s_vb->Unlock();
        }
        s_dev->SetVertexDeclaration(s_decl);
        s_dev->SetStreamSource(0, s_vb, 0, sizeof(Vertex));
        s_dev->SetVertexShader(s_vs);

        // passe 1 : quads couleur
        s_dev->SetPixelShader(s_psColor);
        s_dev->SetTexture(0, NULL);
        if (s_nColor > 0)
            s_dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_nColor / 3);

        // passe 2 : images (une texture par commande)
        s_dev->SetPixelShader(s_psTex);
        for (int i = 0; i < s_nImgCmds; ++i) {
            s_dev->SetTexture(0, s_imgCmds[i].tex);
            s_dev->DrawPrimitive(D3DPT_TRIANGLELIST,
                                 s_nColor + s_imgCmds[i].startVert,
                                 s_imgCmds[i].vertCount / 3);
        }

        // passe 3 : texte
        s_dev->SetTexture(0, s_fontTex);
        if (s_nText > 0)
            s_dev->DrawPrimitive(D3DPT_TRIANGLELIST, s_nColor + s_nImg, s_nText / 3);
        s_dev->SetTexture(0, NULL);
    }
    s_dev->EndScene();
    s_dev->Present(NULL, NULL, NULL, NULL);
}

} // namespace Render
