// XboxHello — homebrew Xbox 360 (XDK)
// "Hello World avance" : rendu D3D9 shaders SM3, texte bitmap embarque,
// lecture des 4 pads XInput, rumble, infos systeme, compteur FPS.
//
// Controles :
//   Stick gauche : deplacer le curseur
//   A            : rumble + compteur
//   BACK + START : quitter (retour au dashboard)

#include <xtl.h>
#include <d3dx9.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "font8x8.h"

#define SCREEN_W   1280
#define SCREEN_H   720
#define MAX_VERTS  (6 * 2048)

// ---------------------------------------------------------------------------
// Ressources D3D
// ---------------------------------------------------------------------------
static IDirect3D9*                 g_pD3D      = NULL;
static IDirect3DDevice9*           g_pDevice   = NULL;
static IDirect3DVertexShader9*     g_pVS       = NULL;
static IDirect3DPixelShader9*      g_pPSColor  = NULL;
static IDirect3DPixelShader9*      g_pPSText   = NULL;
static IDirect3DVertexDeclaration9* g_pDecl    = NULL;
static IDirect3DVertexBuffer9*     g_pVB       = NULL;
static IDirect3DTexture9*          g_pFontTex  = NULL;

// Vertex : position deja en clip-space (calcul CPU), couleur + uv.
struct Vertex {
    float x, y, z, w;
    DWORD color;
    float u, v;
};

// Batches reconstruits a chaque frame : d'abord les quads colores, puis le texte.
static Vertex g_colorVerts[MAX_VERTS];
static int    g_nColorVerts = 0;
static Vertex g_textVerts[MAX_VERTS];
static int    g_nTextVerts = 0;

// ---------------------------------------------------------------------------
// Helpers rendu
// ---------------------------------------------------------------------------
static inline float ClipX(float px) { return (px / (float)SCREEN_W) * 2.0f - 1.0f; }
static inline float ClipY(float py) { return 1.0f - (py / (float)SCREEN_H) * 2.0f; }

static void SetVert(Vertex* v, float px, float py, DWORD color, float u, float vv)
{
    v->x = ClipX(px); v->y = ClipY(py); v->z = 0.5f; v->w = 1.0f;
    v->color = color; v->u = u; v->v = vv;
}

// Ajoute un quad (2 triangles) avec une couleur par coin + rect UV optionnel.
static void PushQuad(Vertex* buf, int& n, float x, float y, float w, float h,
                     DWORD cTL, DWORD cTR, DWORD cBR, DWORD cBL,
                     float u0 = 0.f, float v0 = 0.f, float u1 = 0.f, float v1 = 0.f)
{
    if (n + 6 > MAX_VERTS) return;
    SetVert(&buf[n+0], x,   y,   cTL, u0, v0);
    SetVert(&buf[n+1], x+w, y,   cTR, u1, v0);
    SetVert(&buf[n+2], x,   y+h, cBL, u0, v1);
    SetVert(&buf[n+3], x+w, y,   cTR, u1, v0);
    SetVert(&buf[n+4], x+w, y+h, cBR, u1, v1);
    SetVert(&buf[n+5], x,   y+h, cBL, u0, v1);
    n += 6;
}

static void FillRect(float x, float y, float w, float h, DWORD color)
{
    PushQuad(g_colorVerts, g_nColorVerts, x, y, w, h, color, color, color, color);
}

// Texte monochrome : un quad 8x8*scale par caractere, texture font 128x48
// (16 cellules par ligne, ASCII 32..126).
static void DrawText(float x, float y, float scale, DWORD color, const char* str)
{
    float cx = x;
    for (const char* p = str; *p && g_nTextVerts + 6 <= MAX_VERTS; ++p) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126) c = '?';
        int idx  = c - 32;
        float tx = (float)((idx % 16) * 8);
        float ty = (float)((idx / 16) * 8);
        PushQuad(g_textVerts, g_nTextVerts, cx, y, 8.f * scale, 8.f * scale,
                 color, color, color, color,
                 tx / 128.f, ty / 48.f, (tx + 8.f) / 128.f, (ty + 8.f) / 48.f);
        cx += 8.f * scale;
    }
}

static void DrawTextF(float x, float y, float scale, DWORD color, const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);
    DrawText(x, y, scale, color, buf);
}

// ---------------------------------------------------------------------------
// Init D3D + shaders + font
// ---------------------------------------------------------------------------
static const char* kVSSrc =
    "struct VI{float4 pos:POSITION;float4 col:COLOR0;float2 uv:TEXCOORD0;};"
    "struct VO{float4 pos:POSITION;float4 col:COLOR0;float2 uv:TEXCOORD0;};"
    "VO main(VI i){VO o;o.pos=i.pos;o.col=i.col;o.uv=i.uv;return o;}";

static const char* kPSColorSrc =
    "float4 main(float4 col:COLOR0):COLOR{return col;}";

static const char* kPSTextSrc =
    "sampler s0:register(s0);"
    "float4 main(float4 col:COLOR0,float2 uv:TEXCOORD0):COLOR{return tex2D(s0,uv)*col;}";

static HRESULT CompileShader(const char* src, const char* profile, void** ppOutShader)
{
    ID3DXBuffer* pCode = NULL;
    ID3DXBuffer* pErr  = NULL;
    HRESULT hr = D3DXCompileShader(src, (UINT)strlen(src), NULL, NULL, "main",
                                   profile, 0, &pCode, &pErr, NULL);
    if (FAILED(hr)) {
        if (pErr) OutputDebugStringA((const char*)pErr->GetBufferPointer());
        if (pErr) pErr->Release();
        return hr;
    }
    if (pErr) pErr->Release();
    if (strcmp(profile, "vs_3_0") == 0)
        hr = g_pDevice->CreateVertexShader((DWORD*)pCode->GetBufferPointer(), (IDirect3DVertexShader9**)ppOutShader);
    else
        hr = g_pDevice->CreatePixelShader((DWORD*)pCode->GetBufferPointer(), (IDirect3DPixelShader9**)ppOutShader);
    pCode->Release();
    return hr;
}

static HRESULT BuildFontTexture()
{
    // LIN_ = layout lineaire : LockRect expose les pixels en ordre raster
    // (le format tiled par defaut produirait des glyphes brouilles)
    HRESULT hr = g_pDevice->CreateTexture(128, 48, 1, 0, D3DFMT_LIN_A8R8G8B8,
                                        D3DPOOL_MANAGED, &g_pFontTex, NULL);
    if (FAILED(hr)) return hr;

    D3DLOCKED_RECT lr;
    if (FAILED(g_pFontTex->LockRect(0, &lr, NULL, 0))) return E_FAIL;
    for (int c = 32; c < 127; ++c) {
        int cx = ((c - 32) % 16) * 8;
        int cy = ((c - 32) / 16) * 8;
        for (int gy = 0; gy < 8; ++gy) {
            unsigned char bits = (unsigned char)font8x8_basic[c][gy];
            DWORD* row = (DWORD*)((BYTE*)lr.pBits + (cy + gy) * lr.Pitch);
            for (int gx = 0; gx < 8; ++gx)
                row[cx + gx] = ((bits >> gx) & 1) ? 0xFFFFFFFF : 0x00FFFFFF;
        }
    }
    g_pFontTex->UnlockRect(0);
    return S_OK;
}

static HRESULT InitD3D()
{
    g_pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return E_FAIL;

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof(pp));
    pp.BackBufferWidth        = SCREEN_W;
    pp.BackBufferHeight       = SCREEN_H;
    pp.BackBufferFormat       = D3DFMT_X8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    pp.EnableAutoDepthStencil = FALSE;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    pp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    HRESULT hr = g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_pDevice);
    if (FAILED(hr)) return hr;

    // Alignement pixels type D3D9 pour le rendu 2D screen-space.
    g_pDevice->SetRenderState(D3DRS_HALFPIXELOFFSET, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE,        FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE,   FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE,       D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND,  D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    g_pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
        { 0, 20, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    if (FAILED(g_pDevice->CreateVertexDeclaration(decl, &g_pDecl))) return E_FAIL;

    if (FAILED(g_pDevice->CreateVertexBuffer(sizeof(Vertex) * MAX_VERTS * 2,
               D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &g_pVB, NULL)))
        return E_FAIL;

    if (FAILED(CompileShader(kVSSrc,      "vs_3_0", (void**)&g_pVS)))      return E_FAIL;
    if (FAILED(CompileShader(kPSColorSrc, "ps_3_0", (void**)&g_pPSColor))) return E_FAIL;
    if (FAILED(CompileShader(kPSTextSrc,  "ps_3_0", (void**)&g_pPSText)))  return E_FAIL;
    if (FAILED(BuildFontTexture())) return E_FAIL;

    return S_OK;
}

// ---------------------------------------------------------------------------
// Infos console (API documentees xam/xapi uniquement)
// ---------------------------------------------------------------------------
static const char* VideoStandardName(DWORD std)
{
    switch (std) {
        case XC_VIDEO_STANDARD_NTSC_M: return "NTSC-M";
        case XC_VIDEO_STANDARD_NTSC_J: return "NTSC-J";
        case XC_VIDEO_STANDARD_PAL_I:  return "PAL";
        default: return "?";
    }
}

static const char* LanguageName(DWORD lang)
{
    switch (lang) {
        case 1: return "English";
        case 2: return "Japanese";
        case 3: return "German";
        case 4: return "French";
        case 5: return "Spanish";
        case 6: return "Italian";
        case 7: return "Korean";
        case 8: return "Chinese (Trad.)";
        case 9: return "Portuguese";
        case 10: return "Chinese (Simp.)";
        case 11: return "Polish";
        case 12: return "Russian";
        default: return "Autre";
    }
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------
static void DrawHUD(const XINPUT_STATE* pads, const BOOL* connected,
                    DWORD fps, DWORD frameCount, int aPressCount)
{
    // Fond : degradé vertical vert Xbox -> noir.
    PushQuad(g_colorVerts, g_nColorVerts, 0, 0, (float)SCREEN_W, (float)SCREEN_H,
             0xFF0F2D14, 0xFF0F2D14, 0xFF030803, 0xFF030803);

    // Bandeau titre.
    FillRect(40, 34, 6, 52, 0xFF9BC848);                       // accent blades-green
    DrawText(62, 30, 3.f, 0xFFFFFFFF, "XBOXHELLO");
    DrawText(62, 60, 1.5f, 0xFF9BC848, "homebrew .xex - XDK / D3D9 / XInput");
    FillRect(40, 96, SCREEN_W - 80, 2, 0xFF2A5A1E);

    // Colonne gauche : infos systeme.
    DrawText(48, 122, 1.5f, 0xFF9BC848, "=== SYSTEME ===");
    XVIDEO_MODE vm;
    XGetVideoMode(&vm);
    DWORD upSec = GetTickCount() / 1000;
    DWORD vcaps = XGetVideoCapabilities();
    DrawTextF(48, 152, 1.5f, 0xFFDDDDDD, "Uptime      : %lum%02lus", upSec / 60, upSec % 60);
    DrawTextF(48, 180, 1.5f, 0xFFDDDDDD, "Video       : %lux%lu %s%s %.0fHz",
              vm.dwDisplayWidth, vm.dwDisplayHeight,
              vm.fIsInterlaced ? "i " : "", vm.fIsWideScreen ? "16:9" : "4:3",
              vm.RefreshRate);
    DrawTextF(48, 208, 1.5f, 0xFFDDDDDD, "Standard    : %s", VideoStandardName(vm.VideoStandard));
    DrawTextF(48, 236, 1.5f, 0xFFDDDDDD, "Caps video  :%s%s%s%s",
              (vcaps & XC_VIDEO_FLAGS_WIDESCREEN)  ? " 16:9"  : "",
              (vcaps & XC_VIDEO_FLAGS_HDTV_720p)   ? " 720p"  : "",
              (vcaps & XC_VIDEO_FLAGS_HDTV_1080i)  ? " 1080i" : "",
              (vcaps & XC_VIDEO_FLAGS_HDTV_480p)   ? " 480p"  : "");
    DrawTextF(48, 264, 1.5f, 0xFFDDDDDD, "Game Region : 0x%08X", XGetGameRegion());
    DrawTextF(48, 292, 1.5f, 0xFFDDDDDD, "Langue      : %lu (%s)",
              XGetLanguage(), LanguageName(XGetLanguage()));

    // Colonne droite : etat des 4 pads.
    DrawText(720, 122, 1.5f, 0xFF9BC848, "=== MANETTES ===");
    for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
        float py = 152.f + i * 72.f;
        if (!connected[i]) {
            DrawTextF(720, py, 1.5f, 0xFF666666, "PAD %d  ---", i + 1);
            continue;
        }
        DrawTextF(720, py, 1.5f, 0xFFFFFFFF, "PAD %d  CONNECTE", i + 1);
        const XINPUT_GAMEPAD& g = pads[i].Gamepad;

        char btns[128] = "";
        struct { WORD bit; const char* name; } map[] = {
            { XINPUT_GAMEPAD_DPAD_UP,        "UP "    },
            { XINPUT_GAMEPAD_DPAD_DOWN,      "DOWN "  },
            { XINPUT_GAMEPAD_DPAD_LEFT,      "LEFT "  },
            { XINPUT_GAMEPAD_DPAD_RIGHT,     "RIGHT " },
            { XINPUT_GAMEPAD_START,          "START " },
            { XINPUT_GAMEPAD_BACK,           "BACK "  },
            { XINPUT_GAMEPAD_LEFT_THUMB,     "LS "    },
            { XINPUT_GAMEPAD_RIGHT_THUMB,    "RS "    },
            { XINPUT_GAMEPAD_LEFT_SHOULDER,  "LB "    },
            { XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB "    },
            { XINPUT_GAMEPAD_A,              "A "     },
            { XINPUT_GAMEPAD_B,              "B "     },
            { XINPUT_GAMEPAD_X,              "X "     },
            { XINPUT_GAMEPAD_Y,              "Y "     },
        };
        for (int b = 0; b < (int)(sizeof(map) / sizeof(map[0])); ++b)
            if (g.wButtons & map[b].bit) strcat_s(btns, sizeof(btns), map[b].name);
        DrawText(740, py + 22, 1.5f, 0xFFFFE08A, btns[0] ? btns : "(aucun bouton)");
        DrawTextF(740, py + 44, 1.5f, 0xFFBBBBBB,
                  "LX:%6d LY:%6d  RX:%6d RY:%6d  LT:%3d RT:%3d",
                  g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY,
                  g.bLeftTrigger, g.bRightTrigger);
    }

    // Stats bas de page + instructions.
    DrawTextF(48, SCREEN_H - 96, 1.5f, 0xFFAAAAAA,
              "FPS: %lu   Frame: %lu   Appuis A (pad 1): %d", fps, frameCount, aPressCount);
    DrawText(48, SCREEN_H - 64, 1.5f, 0xFF9BC848,
             "Stick gauche : bouger le curseur   |   A : rumble   |   BACK+START : quitter");
    FillRect(40, SCREEN_H - 40, SCREEN_W - 80, 2, 0xFF2A5A1E);
}

static void DrawCursor(float x, float y)
{
    FillRect(x - 14, y - 14, 28, 28, 0x809BC848);
    FillRect(x - 14, y - 14, 28, 2,  0xFF9BC848);
    FillRect(x - 14, y + 12, 28, 2,  0xFF9BC848);
    FillRect(x - 14, y - 14, 2,  28, 0xFF9BC848);
    FillRect(x + 12, y - 14, 2,  28, 0xFF9BC848);
}

static void Render()
{
    void* pData = NULL;
    if (FAILED(g_pVB->Lock(0, 0, &pData, 0))) return;
    memcpy(pData, g_colorVerts, g_nColorVerts * sizeof(Vertex));
    memcpy((BYTE*)pData + g_nColorVerts * sizeof(Vertex),
           g_textVerts, g_nTextVerts * sizeof(Vertex));
    g_pVB->Unlock();

    g_pDevice->BeginScene();
    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
    g_pDevice->SetVertexDeclaration(g_pDecl);
    g_pDevice->SetStreamSource(0, g_pVB, 0, sizeof(Vertex));
    g_pDevice->SetVertexShader(g_pVS);

    g_pDevice->SetPixelShader(g_pPSColor);
    g_pDevice->SetTexture(0, NULL);
    if (g_nColorVerts > 0)
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, g_nColorVerts / 3);

    g_pDevice->SetPixelShader(g_pPSText);
    g_pDevice->SetTexture(0, g_pFontTex);
    if (g_nTextVerts > 0)
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, g_nColorVerts, g_nTextVerts / 3);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// ---------------------------------------------------------------------------
// Point d'entree
// ---------------------------------------------------------------------------
void __cdecl main()
{
    if (FAILED(InitD3D())) {
        OutputDebugStringA("[XboxHello] Echec InitD3D\n");
        return;
    }
    OutputDebugStringA("[XboxHello] Demarrage OK\n");

    XINPUT_STATE pads[XUSER_MAX_COUNT];
    BOOL connected[XUSER_MAX_COUNT] = { FALSE };
    WORD prevButtons[XUSER_MAX_COUNT] = { 0 };
    DWORD rumbleEnd[XUSER_MAX_COUNT] = { 0 };

    float cursorX = SCREEN_W / 2.f, cursorY = SCREEN_H / 2.f;
    int   aPressCount = 0;
    DWORD frameCount = 0, totalFrames = 0, fps = 0, fpsStart = GetTickCount();
    DWORD lastTick = GetTickCount();
    BOOL running = TRUE;

    while (running) {
        DWORD now  = GetTickCount();
        float dt   = (now - lastTick) / 1000.f;
        lastTick   = now;
        if (dt > 0.1f) dt = 0.1f;

        // --- Input ---
        for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
            connected[i] = (XInputGetState(i, &pads[i]) == ERROR_SUCCESS);
            if (!connected[i]) { prevButtons[i] = 0; continue; }

            const XINPUT_GAMEPAD& g = pads[i].Gamepad;

            // Front montant de A : rumble + compteur.
            if ((g.wButtons & XINPUT_GAMEPAD_A) && !(prevButtons[i] & XINPUT_GAMEPAD_A)) {
                XINPUT_VIBRATION vib;
                vib.wLeftMotorSpeed  = 48000;
                vib.wRightMotorSpeed = 48000;
                XInputSetState(i, &vib);
                rumbleEnd[i] = now + 300;
                if (i == 0) ++aPressCount;
            }
            if (rumbleEnd[i] && now > rumbleEnd[i]) {
                XINPUT_VIBRATION off = { 0, 0 };
                XInputSetState(i, &off);
                rumbleEnd[i] = 0;
            }

            // Pad 1 controle le curseur.
            if (i == 0) {
                cursorX += (g.sThumbLX / 32767.f) * 600.f * dt;
                cursorY -= (g.sThumbLY / 32767.f) * 600.f * dt;
                if (cursorX < 20) cursorX = 20; if (cursorX > SCREEN_W - 20)  cursorX = SCREEN_W - 20.f;
                if (cursorY < 20) cursorY = 20; if (cursorY > SCREEN_H - 20) cursorY = SCREEN_H - 20.f;
            }

            // BACK + START = quitter.
            if ((g.wButtons & XINPUT_GAMEPAD_BACK) && (g.wButtons & XINPUT_GAMEPAD_START))
                running = FALSE;

            prevButtons[i] = g.wButtons;
        }

        // --- FPS ---
        ++frameCount;
        ++totalFrames;
        if (now - fpsStart >= 1000) { fps = frameCount; frameCount = 0; fpsStart = now; }

        // --- Scene ---
        g_nColorVerts = 0;
        g_nTextVerts  = 0;
        DrawHUD(pads, connected, fps, totalFrames, aPressCount);
        DrawCursor(cursorX, cursorY);
        Render();
    }

    // Cleanup propre avant retour au dashboard.
    if (g_pFontTex) g_pFontTex->Release();
    if (g_pVB)      g_pVB->Release();
    if (g_pDecl)    g_pDecl->Release();
    if (g_pVS)      g_pVS->Release();
    if (g_pPSColor) g_pPSColor->Release();
    if (g_pPSText)  g_pPSText->Release();
    if (g_pDevice)  g_pDevice->Release();
    if (g_pD3D)     g_pD3D->Release();
}
