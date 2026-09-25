// renderer.h — mini renderer D3D9 Xbox 360 (2D screen-space, shaders SM3)
// Quads colores + texte bitmap + images texturees (posters).
#pragma once
#include <xtl.h>
#include <d3dx9.h>

namespace Render {

HRESULT Init(IDirect3DDevice9* device);   // cree shaders, VB, texture font
void    Shutdown();

void    BeginFrame();                     // BeginScene + Clear
void    FlushBatches();                   // reset les batches de quads
void    EndFrame();                       // upload VB + draw calls + Present

// --- Primitives couleur ---
void    FillRect(float x, float y, float w, float h, DWORD color);
void    FillRectGradV(float x, float y, float w, float h, DWORD cTop, DWORD cBottom);
void    RectOutline(float x, float y, float w, float h, float t, DWORD color);

// --- Texte (font 8x8, scale en pixels par colonne) ---
void    Text(float x, float y, float scale, DWORD color, const char* str);
void    TextF(float x, float y, float scale, DWORD color, const char* fmt, ...);
float   TextWidth(const char* str, float scale);
// Coupe le texte avec "..." si plus large que maxWidthPx
void    TextEllipsis(float x, float y, float scale, DWORD color, const char* str, float maxWidthPx);

// --- Image texturee (poster) : chaque appel = sa propre texture ---
void    Image(IDirect3DTexture9* tex, float x, float y, float w, float h, DWORD tint = 0xFFFFFFFF);

}
