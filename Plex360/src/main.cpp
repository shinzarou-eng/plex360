// Plex360 — client Plex pour Xbox 360 (homebrew XDK)
// Ecrans : BOOT -> SECTIONS -> BROWSE (grille posters) -> DETAIL -> PHOTO
// Reseau : worker thread (sockets BSD), parsing/rendu sur thread principal.
//
// Config : config.ini a cote du .xex (game:), sinon usb0:/hdd1:
//   server=192.168.1.10
//   port=32400
//   token=xxxx          (optionnel si LAN autorise sans auth dans Plex)

#include <xtl.h>
#include <xaudio2.h>
#include <xmedia2.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <vector>
#include "renderer.h"
#include "net.h"
#include "plex.h"

#define SCREEN_W 1280
#define SCREEN_H 720

#define COLS        5
#define ROWS_VIS    2
#define POSTER_W    212
#define POSTER_H    318
#define CELL_W      232
#define GRID_X      40
#define GRID_Y      150
#define ROW_H       372

#define ACCENT   0xFF9BC848   // vert blades
#define TXT_MAIN 0xFFEEEEEE
#define TXT_DIM  0xFF999999

// IDs de jobs reseau : bits hauts = type, ticket anti-stale en octet bas
#define JOB_PHOTO     0xFFFFFFFE
#define JOB_SECTIONS  0xFFFFFFFD
#define LIST_ID(t)    (0x80000000u | ((t) & 0xFFu))
#define POSTER_ID(t,i)(0x40000000u | (((t) & 0xFFu) << 16) | ((i) & 0xFFFFu))
#define JOB_TICKET(r) ((r) & 0xFFu)

// ---------------------------------------------------------------------------
// D3D
// ---------------------------------------------------------------------------
static IDirect3D9*       g_d3d    = NULL;
static IDirect3DDevice9* g_device = NULL;
static IXAudio2*         g_xaudio = NULL;

static HRESULT InitD3D()
{
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) return E_FAIL;
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
    return g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, NULL,
                               D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &g_device);
}

// ---------------------------------------------------------------------------
// Job queue reseau (main -> worker -> main)
// ---------------------------------------------------------------------------
struct NetJob    { DWORD id; char path[1024]; };
struct NetResult { DWORD id; BYTE* data; DWORD size; DWORD status; bool ok; };

static CRITICAL_SECTION      s_qLock;
static std::vector<NetJob>   s_jobs;
static std::vector<NetResult> s_results;
static volatile bool s_workerRun = true;
static volatile LONG s_inFlight  = 0;

static void QueueJob(DWORD id, const char* path)
{
    NetJob j; j.id = id;
    strcpy_s(j.path, sizeof(j.path), path);
    EnterCriticalSection(&s_qLock);
    s_jobs.push_back(j);
    LeaveCriticalSection(&s_qLock);
    InterlockedIncrement(&s_inFlight);
}

static DWORD WINAPI WorkerProc(LPVOID)
{
    while (s_workerRun) {
        NetJob j;
        bool has = false;
        EnterCriticalSection(&s_qLock);
        if (!s_jobs.empty()) { j = s_jobs.front(); s_jobs.erase(s_jobs.begin()); has = true; }
        LeaveCriticalSection(&s_qLock);
        if (!has) { Sleep(10); continue; }

        NetResult r;
        r.id = j.id; r.data = NULL; r.size = 0; r.status = 0;
        r.ok = Net::HttpGet(Plex::Host(), Plex::Port(), j.path,
                            Plex::ExtraHeaders(), &r.data, &r.size, &r.status);
        EnterCriticalSection(&s_qLock);
        s_results.push_back(r);
        LeaveCriticalSection(&s_qLock);
        InterlockedDecrement(&s_inFlight);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Etat appli
// ---------------------------------------------------------------------------
enum Screen { SCR_BOOT, SCR_CONFIG_ERR, SCR_NET_ERR, SCR_SECTIONS, SCR_BROWSE, SCR_DETAIL, SCR_PHOTO };

struct BrowseCtx {
    bool bySection;               // true: key=section, false: key=chemin enfants
    char key[PLEX_MAX_PATH];
    char title[PLEX_MAX_TITLE];   // breadcrumb
};
struct NavEntry { Screen scr; BrowseCtx ctx; int sel; };

static Screen                s_scr = SCR_BOOT;
static std::vector<NavEntry> s_nav;

static PlexSection s_sections[64];
static int         s_nSections = 0;
static int         s_secSel    = 0;
static bool        s_sectionsLoaded = false;
static bool        s_sectionsAsked  = false;

static PlexItem  s_items[PLEX_MAX_ITEMS];
static int       s_nItems = 0, s_sel = 0, s_scrollRow = 0;
static IDirect3DTexture9* s_thumbs[PLEX_MAX_ITEMS];
static bool      s_thumbAsked[PLEX_MAX_ITEMS];
static bool      s_listLoading = false;
static DWORD     s_listTicket = 0;      // incremente a chaque RequestList
static BrowseCtx s_browse;
static char      s_errorMsg[256] = "";

static IDirect3DTexture9* s_photoTex = NULL;
static bool s_photoLoading = false;

static XINPUT_STATE s_pad;
static WORD s_prevButtons = 0;
static bool s_padOn = false;
static bool s_quit  = false;

// ---------------------------------------------------------------------------
static void SetError(Screen scr, const char* msg)
{
    strcpy_s(s_errorMsg, sizeof(s_errorMsg), msg);
    s_scr = scr;
}

static void ClearThumbs()
{
    for (int i = 0; i < PLEX_MAX_ITEMS; ++i)
        if (s_thumbs[i]) { s_thumbs[i]->Release(); s_thumbs[i] = NULL; }
    memset(s_thumbAsked, 0, sizeof(s_thumbAsked));
}

static void RequestList(const BrowseCtx& ctx, int restoreSel = 0)
{
    ClearThumbs();
    ++s_listTicket;
    s_nItems = 0; s_scrollRow = 0;
    s_sel = restoreSel;
    s_browse = ctx;
    s_listLoading = true;
    char path[1024];
    if (ctx.bySection) Plex::PathSectionItems(ctx.key, path, sizeof(path));
    else               strcpy_s(path, sizeof(path), ctx.key);
    QueueJob(LIST_ID(s_listTicket), path);
    s_scr = SCR_BROWSE;
}

static void PushNav(Screen scr, const BrowseCtx* ctx, int sel)
{
    NavEntry e; e.scr = scr; e.sel = sel;
    if (ctx) e.ctx = *ctx; else memset(&e.ctx, 0, sizeof(e.ctx));
    s_nav.push_back(e);
}

static void GoBack()
{
    if (s_scr == SCR_PHOTO) { s_scr = SCR_DETAIL; return; }
    if (s_scr == SCR_DETAIL) { s_scr = SCR_BROWSE; return; }
    if (s_nav.empty()) return;
    NavEntry e = s_nav.back(); s_nav.pop_back();
    if (e.scr == SCR_SECTIONS) { s_scr = SCR_SECTIONS; s_secSel = e.sel; }
    else RequestList(e.ctx, e.sel);
}

// ---------------------------------------------------------------------------
// Config : game:\config.ini puis usb0:/hdd1:
// ---------------------------------------------------------------------------
static bool LoadConfig()
{
    const char* paths[] = {
        "game:\\config.ini", "usb0:\\config.ini", "hdd1:\\config.ini",
        "usb1:\\config.ini", "usb2:\\config.ini"
    };
    char host[64] = "", token[128] = "";
    int port = 32400;
    bool found = false;
    for (int i = 0; i < 5 && !found; ++i) {
        FILE* f = fopen(paths[i], "rb");
        if (!f) continue;
        found = true;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            char* val = eq + 1;
            while (*val == ' ') ++val;
            char* end = val + strlen(val);
            while (end > val && (end[-1] == '\r' || end[-1] == '\n' || end[-1] == ' ')) *--end = 0;
            if      (_stricmp(line, "server") == 0) strcpy_s(host, sizeof(host), val);
            else if (_stricmp(line, "port") == 0)   port = atoi(val);
            else if (_stricmp(line, "token") == 0)  strcpy_s(token, sizeof(token), val);
        }
        fclose(f);
    }
    if (!found || !host[0]) return false;
    Plex::SetServer(host, (WORD)port, token);
    return true;
}

// ---------------------------------------------------------------------------
// Resultats reseau -> thread principal
// ---------------------------------------------------------------------------
static void ApplyResult(const NetResult& r)
{
    if (r.id == JOB_SECTIONS) {
        if (r.ok && r.data) {
            s_nSections = Plex::ParseSections((const char*)r.data, s_sections, 64);
            s_sectionsLoaded = true;
        } else {
            char msg[160];
            sprintf_s(msg, sizeof(msg), "HTTP %lu sections (etape %d err %d)",
                      r.status, Net::LastStage(), Net::LastErr());
            SetError(SCR_NET_ERR, msg);
        }
        return;
    }
    if ((r.id & 0xFF000000u) == 0x80000000u) {       // liste d'items
        if (JOB_TICKET(r.id) != (s_listTicket & 0xFF)) return; // stale
        s_listLoading = false;
        if (r.ok && r.data) {
            s_nItems = Plex::ParseItems((const char*)r.data, s_items, PLEX_MAX_ITEMS);
            if (s_sel >= s_nItems) s_sel = s_nItems > 0 ? s_nItems - 1 : 0;
            int selRow = s_sel / COLS;
            if (selRow >= s_scrollRow + ROWS_VIS) s_scrollRow = selRow - ROWS_VIS + 1;
        } else {
            char msg[160];
            sprintf_s(msg, sizeof(msg), "HTTP %lu liste (etape %d err %d)",
                      r.status, Net::LastStage(), Net::LastErr());
            SetError(SCR_NET_ERR, msg);
        }
        return;
    }
    if (r.id == JOB_PHOTO) {
        s_photoLoading = false;
        if (r.ok && r.data) {
            IDirect3DTexture9* t = NULL;
            if (SUCCEEDED(D3DXCreateTextureFromFileInMemoryEx(
                    g_device, r.data, r.size, D3DX_DEFAULT, D3DX_DEFAULT, 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                    D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &t)))
                s_photoTex = t;
        }
        return;
    }
    if ((r.id & 0xFF000000u) == 0x40000000u) {       // poster
        if (JOB_TICKET(r.id >> 16) != (s_listTicket & 0xFF)) return; // stale
        int idx = (int)(r.id & 0xFFFF);
        if (idx < PLEX_MAX_ITEMS && r.ok && r.data) {
            IDirect3DTexture9* t = NULL;
            if (SUCCEEDED(D3DXCreateTextureFromFileInMemoryEx(
                    g_device, r.data, r.size, POSTER_W, POSTER_H, 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                    D3DX_FILTER_LINEAR, D3DX_FILTER_LINEAR, 0, NULL, NULL, &t)))
                s_thumbs[idx] = t;
        }
    }
}

static void DrainResults()
{
    for (;;) {
        NetResult r; bool has = false;
        EnterCriticalSection(&s_qLock);
        if (!s_results.empty()) { r = s_results.front(); s_results.erase(s_results.begin()); has = true; }
        LeaveCriticalSection(&s_qLock);
        if (!has) break;
        ApplyResult(r);
        if (r.data) free(r.data);
    }
}

// Demande les posters visibles manquants (max ~10 en vol)
static void RequestVisibleThumbs()
{
    if (s_scr != SCR_BROWSE) return;
    int first = s_scrollRow * COLS;
    int last  = first + COLS * ROWS_VIS;
    if (last > s_nItems) last = s_nItems;
    for (int i = first; i < last; ++i) {
        if (s_thumbs[i] || s_thumbAsked[i] || !s_items[i].thumb[0]) continue;
        if (s_inFlight >= 8) break;
        char path[1200];
        Plex::PathTranscodeThumb(s_items[i].thumb, POSTER_W, POSTER_H, path, sizeof(path));
        QueueJob(POSTER_ID(s_listTicket, i), path);
        s_thumbAsked[i] = true;
    }
}

// ---------------------------------------------------------------------------
// Lecture video (XMV/WMV via IXMedia2XmvPlayer — decodage+rendu+A/V sync
// geres par le player; Play() bloque jusqu'a fin ou Stop()).
// ---------------------------------------------------------------------------
static volatile bool s_videoCancel = false;

static void VideoInputCb(PVOID ctx)
{
    // appelee une fois par frame par le player; B = stop, A = pause/resume
    IXMedia2XmvPlayer* p = (IXMedia2XmvPlayer*)ctx;
    static WORD prev = 0;
    XINPUT_STATE st;
    WORD b = (XInputGetState(0, &st) == ERROR_SUCCESS) ? st.Gamepad.wButtons : 0;
    WORD pressed = b & ~prev;
    prev = b;
    if (pressed & XINPUT_GAMEPAD_B) s_videoCancel = true;
    if (pressed & XINPUT_GAMEPAD_A) {
        XMEDIA_PLAYBACK_STATUS s;
        if (SUCCEEDED(p->GetStatus(&s))) {
            if (s.Status == XMEDIA_PLAYER_PAUSED) p->Resume();
            else if (s.Status == XMEDIA_PLAYER_PLAYING) p->Pause();
        }
    }
}

static char s_videoErr[160] = "";

// Joue un fichier video local (wmv/asf). Retourne true si lecture OK.
static bool PlayVideoFile(const char* filePath)
{
    if (!g_xaudio) { strcpy_s(s_videoErr, sizeof(s_videoErr), "XAudio2 absent"); return false; }
    IXMedia2XmvPlayer* player = NULL;
    XMEDIA_XMV_CREATE_PARAMETERS prm;
    ZeroMemory(&prm, sizeof(prm));
    prm.createType = XMEDIA_CREATE_FROM_FILE;
    prm.createFromFile.szFileName = filePath;
    prm.dwAudioStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    prm.dwVideoStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    HRESULT hr = XMedia2CreateXmvPlayer(g_device, g_xaudio, &prm, &player);
    if (FAILED(hr) || !player) {
        sprintf_s(s_videoErr, sizeof(s_videoErr), "XmvPlayer KO 0x%08lX", hr);
        return false;
    }
    s_videoErr[0] = 0;

    s_videoCancel = false;
    player->SetCallback(XMEDIA_NOTIFY_END_OF_FRAME, VideoInputCb, player);
    player->Play(0, 0); // bloquant jusqu'a fin / Stop
    if (s_videoCancel) player->Stop(XMEDIA_STOP_IMMEDIATE);
    player->Release();

    // le player touche aux etats D3D
    if (g_device) g_device->SetRenderState(D3DRS_VIEWPORTENABLE, TRUE);
    return true;
}

// --- Streaming via PlexRelay (PC) : transcode WMV3 + ranges HTTP ---
#define RELAY_PORT 8090

struct StreamCtx {
    int  id;
    char host[64];
    WORD port;
    char tag;   // 'A' ou 'V' : quel callback le player a invoque
    int  nCalls;
    int  nFail;
};

static char s_videoDiag[1100] = "";
static int  s_diagLen = 0;

static void DiagAdd(const char* fmt, ...)
{
    if (s_diagLen >= (int)sizeof(s_videoDiag) - 80) return;
    va_list ap; va_start(ap, fmt);
    int n = _vsnprintf_s(s_videoDiag + s_diagLen, sizeof(s_videoDiag) - s_diagLen,
                         _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) s_diagLen += n;
}

// Callback USER_IO du player XMV : lit offset/len via GET /read du relay.
static HRESULT CALLBACK StreamReadCb(void* pv, ULONGLONG off, void* buf,
                                     DWORD bytes, DWORD* read)
{
    StreamCtx* c = (StreamCtx*)pv;
    char path[160];
    sprintf_s(path, sizeof(path), "/read?id=%d&off=%I64u&len=%lu",
              c->id, off, bytes);
    BYTE* d = NULL; DWORD sz = 0, st = 0;
    bool ok = Net::HttpGet(c->host, c->port, path, NULL, &d, &sz, &st);
    DWORD n = (ok && d) ? min(sz, bytes) : 0;
    if (n) memcpy(buf, d, n);
    if (d) free(d);
    *read = n;
    if (c->nCalls < 8 || n == 0)
        DiagAdd("%c#%d off=%I64u len=%lu ret=%lu\n", c->tag, c->nCalls, off, bytes, n);
    c->nCalls++;
    if (!n) c->nFail++;
    return S_OK;
}

// Callback USER_IO qui lit un fichier local a un offset (test du mode stream).
static HRESULT CALLBACK FileReadCb(void* pv, ULONGLONG off, void* buf,
                                   DWORD bytes, DWORD* read)
{
    HANDLE h = (HANDLE)pv;
    LARGE_INTEGER li; li.QuadPart = off;
    DWORD n = 0;
    if (SetFilePointerEx(h, li, NULL, FILE_BEGIN))
        ReadFile(h, buf, bytes, &n, NULL);
    *read = n;
    return S_OK;
}

// Joue un fichier local en USER_IO (meme stream pour audio+video).
// Isole la semantique USER_IO du reseau : si KO ici aussi, c'est l'usage.
static bool PlayVideoUserIo(const char* filePath)
{
    if (!g_xaudio) { strcpy_s(s_videoErr, sizeof(s_videoErr), "XAudio2 absent"); return false; }
    HANDLE h = CreateFileA(filePath, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        sprintf_s(s_videoErr, sizeof(s_videoErr), "ouverture KO err=%lu", GetLastError());
        return false;
    }
    IXMedia2XmvPlayer* player = NULL;
    XMEDIA_XMV_CREATE_PARAMETERS prm;
    ZeroMemory(&prm, sizeof(prm));
    prm.createType = XMEDIA_CREATE_FROM_USER_IO;
    prm.dwFlags = XMEDIA_CREATE_SHARE_IO_CACHE;
    prm.createFromUserIo.pfnAudioStreamReadCallback = FileReadCb;
    prm.createFromUserIo.pfnVideoStreamReadCallback = FileReadCb;
    prm.createFromUserIo.pvAudioStreamContext = (void*)h;
    prm.createFromUserIo.pvVideoStreamContext = (void*)h;
    prm.dwAudioStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    prm.dwVideoStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    HRESULT hr = XMedia2CreateXmvPlayer(g_device, g_xaudio, &prm, &player);
    if (FAILED(hr) || !player) {
        sprintf_s(s_videoErr, sizeof(s_videoErr), "UserIO local KO 0x%08lX", hr);
        CloseHandle(h);
        return false;
    }
    s_videoErr[0] = 0;
    s_videoCancel = false;
    player->SetCallback(XMEDIA_NOTIFY_END_OF_FRAME, VideoInputCb, player);
    player->Play(0, 0);
    if (s_videoCancel) player->Stop(XMEDIA_STOP_IMMEDIATE);
    player->Release();
    CloseHandle(h);
    if (g_device) g_device->SetRenderState(D3DRS_VIEWPORTENABLE, TRUE);
    return true;
}
static char* RelayGet(const char* path)
{
    BYTE* d = NULL; DWORD sz = 0, st = 0;
    if (!Net::HttpGet(Plex::Host(), RELAY_PORT, path, NULL, &d, &sz, &st) || !d)
        return NULL;
    char* s = (char*)malloc(sz + 1);
    memcpy(s, d, sz); s[sz] = 0;
    free(d);
    return s;
}

// Joue un media Plex via le relay (transcode WMV cote PC + lecture stream).
static bool PlayStreamedVideo(const PlexItem& it)
{
    if (!g_xaudio)          { strcpy_s(s_videoErr, sizeof(s_videoErr), "XAudio2 absent"); return false; }
    if (!it.partFile[0])    { strcpy_s(s_videoErr, sizeof(s_videoErr), "pas de Part.file"); return false; }

    char enc[600];
    Plex::UrlEncode(it.partFile, enc, sizeof(enc));
    char path[800];
    sprintf_s(path, sizeof(path), "/start?file=%s", enc);
    char* r = RelayGet(path);
    int id = -1;
    if (r) { sscanf(r, "id=%d", &id); free(r); }
    if (id < 0) { strcpy_s(s_videoErr, sizeof(s_videoErr), "relay KO (:8090 actif ?)"); return false; }

    s_videoDiag[0] = 0; s_diagLen = 0;
    StreamCtx ctx;
    ctx.id = id; strcpy_s(ctx.host, sizeof(ctx.host), Plex::Host());
    ctx.port = RELAY_PORT; ctx.tag = 'S'; ctx.nCalls = 0; ctx.nFail = 0;

    // Attente buffer initial (~512 Ko) avec ecran de progression
    bool ready = false;
    for (int i = 0; i < 600 && !ready; ++i) {
        sprintf_s(path, sizeof(path), "/status?id=%d", id);
        char* s = RelayGet(path);
        if (!s) { sprintf_s(s_videoErr, sizeof(s_videoErr), "relay perdu"); break; }
        long written = 0; int done = 0; char* ep = strstr(s, "err=");
        if (ep && ep[4] && strncmp(ep + 4, "(null)", 6) != 0 &&
            strncmp(ep + 4, "\n", 1) != 0 && ep[4] != '\r') {
            const char* e2 = ep + 4;
            if (*e2) { sprintf_s(s_videoErr, sizeof(s_videoErr), "relay: %.80s", e2); }
        }
        sscanf(s, "written=%ld done=%d", &written, &done);
        free(s);
        if (s_videoErr[0]) break;
        if (done || written > 512 * 1024) ready = true;

        Render::BeginFrame();
        Render::FillRectGradV(0, 0, SCREEN_W, SCREEN_H, 0xFF0F2D14, 0xFF030803);
        Render::Text(64, 300, 2.f, TXT_MAIN, "Preparation video...");
        Render::TextF(64, 350, 1.5f, TXT_DIM, "transcodage: %ld Mo", written / 1048576);
        Render::EndFrame();
        Sleep(400);
    }
    if (!ready) {
        sprintf_s(path, sizeof(path), "/stop?id=%d", id);
        char* s = RelayGet(path); if (s) free(s);
        if (!s_videoErr[0]) strcpy_s(s_videoErr, sizeof(s_videoErr), "transcodage trop lent");
        return false;
    }

    Net::SetIoTimeout(60000); // le relay peut mettre du temps a produire
    IXMedia2XmvPlayer* player = NULL;
    XMEDIA_XMV_CREATE_PARAMETERS prm;
    ZeroMemory(&prm, sizeof(prm));
    prm.createType = XMEDIA_CREATE_FROM_USER_IO;
    // SHARE_IO_CACHE + meme callback/contexte => un seul flux partage,
    // le player demultiplexe lui-meme le conteneur ASF.
    prm.dwFlags = XMEDIA_CREATE_SHARE_IO_CACHE;
    prm.createFromUserIo.pfnAudioStreamReadCallback = StreamReadCb;
    prm.createFromUserIo.pfnVideoStreamReadCallback = StreamReadCb;
    prm.createFromUserIo.pvAudioStreamContext = &ctx;
    prm.createFromUserIo.pvVideoStreamContext = &ctx;
    prm.dwAudioStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    prm.dwVideoStreamId = XMEDIA_STREAM_ID_USE_DEFAULT;
    HRESULT hr = XMedia2CreateXmvPlayer(g_device, g_xaudio, &prm, &player);
    bool ok = false;
    if (SUCCEEDED(hr) && player) {
        DiagAdd("player OK (%d lectures, %d vides)\n", ctx.nCalls, ctx.nFail);
        s_videoCancel = false;
        player->SetCallback(XMEDIA_NOTIFY_END_OF_FRAME, VideoInputCb, player);
        player->Play(0, 0);
        if (s_videoCancel) player->Stop(XMEDIA_STOP_IMMEDIATE);
        player->Release();
        ok = true;
    } else {
        sprintf_s(s_videoErr, sizeof(s_videoErr), "XmvPlayer stream KO 0x%08lX", hr);
    }
    Net::SetIoTimeout(10000);
    sprintf_s(path, sizeof(path), "/stop?id=%d", id);
    char* s = RelayGet(path); if (s) free(s);
    if (g_device) g_device->SetRenderState(D3DRS_VIEWPORTENABLE, TRUE);
    return ok;
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static bool Pressed(WORD btn)
{
    return s_padOn && (s_pad.Gamepad.wButtons & btn) && !(s_prevButtons & btn);
}

// ---------------------------------------------------------------------------
// Rendu ecrans
// ---------------------------------------------------------------------------
static void DrawHeader(const char* title)
{
    Render::FillRectGradV(0, 0, SCREEN_W, SCREEN_H, 0xFF0F2D14, 0xFF030803);
    Render::FillRect(40, 40, 8, 40, ACCENT);
    Render::Text(64, 38, 3.f, TXT_MAIN, title);
    Render::FillRect(40, 96, SCREEN_W - 80, 2, 0xFF2A5A1E);
    Render::Text(SCREEN_W - 200, 44, 1.5f, TXT_DIM, "PLEX360");
}

static void DrawFooter(const char* hints)
{
    Render::FillRect(40, SCREEN_H - 52, SCREEN_W - 80, 2, 0xFF2A5A1E);
    Render::Text(48, SCREEN_H - 42, 1.5f, ACCENT, hints);
    Render::TextF(48, SCREEN_H - 20, 1.3f, TXT_DIM, "Console %s -> %s:%u",
                  Net::LocalIPStr()[0] ? Net::LocalIPStr() : "?",
                  Plex::Host(), Plex::Port());
    if (s_inFlight > 0)
        Render::TextF(SCREEN_W - 220, SCREEN_H - 42, 1.5f, TXT_DIM, "Reseau: %ld", s_inFlight);
}

static void ScreenBoot()
{
    DrawHeader("PLEX360");
    Render::Text(64, 200, 2.f, TXT_MAIN, "Connexion au serveur Plex...");
}

static void ScreenConfigErr()
{
    DrawHeader("PLEX360");
    Render::Text(64, 160, 2.f, 0xFFFF6666, "config.ini introuvable");
    Render::Text(64, 210, 1.5f, TXT_MAIN, "Cree un fichier config.ini a cote de Plex360.xex :");
    Render::Text(96, 260, 1.5f, TXT_DIM, "server=192.168.1.10");
    Render::Text(96, 290, 1.5f, TXT_DIM, "port=32400");
    Render::Text(96, 320, 1.5f, TXT_DIM, "token=xxxxxxxx   (optionnel)");
    Render::Text(64, 380, 1.5f, TXT_DIM, "Puis relance l'app.");
    DrawFooter("BACK+START : quitter");
}

static char s_probeLines[6][120];
static int  s_probeCount = 0;
static bool s_probing    = false;

static void RunProbes()
{
    s_probing = true; s_probeCount = 0;
    struct { const char* label; const char* ip; WORD port; } tests[] = {
        { "PC Plex 32400",  "192.168.129.177", 32400 },
        { "PC test  8080",  "192.168.129.177", 8080  },
        { "PC SMB   445",   "192.168.129.177", 445   },
        { "Box .129.1  :80","192.168.129.1",   80    },
        { "Box .129.254:80","192.168.129.254", 80    },
    };
    for (int i = 0; i < 5; ++i) {
        bool ok = Net::Probe(tests[i].ip, tests[i].port, 3000);
        sprintf_s(s_probeLines[s_probeCount], sizeof(s_probeLines[0]),
                  "%s : %s (err %d)", tests[i].label,
                  ok ? "OK" : (Net::LastErr() == 10061 ? "REFUSE" : "TIMEOUT"),
                  Net::LastErr());
        s_probeCount++;
    }
    s_probing = false;
}

static void DrawWrapped(float x, float y, float scale, DWORD color,
                        const char* text, float maxW, int maxLines);

static void ScreenNetErr()
{
    DrawHeader("PLEX360");
    Render::Text(64, 160, 2.f, 0xFFFF6666, "Erreur reseau");
    Render::Text(64, 210, 1.5f, TXT_MAIN, s_errorMsg);
    Render::Text(64, 250, 1.5f, TXT_DIM, "Verifie server/port dans config.ini et que Plex tourne.");
    if (s_probing)
        Render::Text(64, 300, 1.5f, ACCENT, "Sondes en cours (~15s)...");
    for (int i = 0; i < s_probeCount; ++i)
        Render::Text(64, 300 + i * 28, 1.5f, TXT_MAIN, s_probeLines[i]);
    DrawFooter("A : reessayer   |   X : sonder le reseau   |   BACK+START : quitter");
}

static void ScreenSections()
{
    DrawHeader("BIBLIOTHEQUES");
    if (!s_sectionsLoaded) {
        Render::Text(64, 160, 2.f, TXT_DIM, "Chargement...");
        return;
    }
    if (s_nSections == 0) {
        Render::Text(64, 160, 2.f, TXT_MAIN, "Aucune bibliotheque trouvee.");
        return;
    }
    float y = 140;
    for (int i = 0; i < s_nSections; ++i) {
        bool sel = (i == s_secSel);
        if (sel) Render::FillRect(56, y - 10, 700, 88, 0xFF1E3D16);
        Render::FillRect(60, y - 4, 6, 76, sel ? ACCENT : 0xFF2A5A1E);
        Render::Text(84, y, 2.2f, sel ? TXT_MAIN : TXT_DIM, s_sections[i].title);
        Render::Text(84, y + 42, 1.4f, TXT_DIM, s_sections[i].type);
        y += 100;
    }
    if (s_videoErr[0])
        Render::Text(64, SCREEN_H - 110, 1.5f, 0xFFFF6666, s_videoErr);
    if (s_videoDiag[0])
        DrawWrapped(64, SCREEN_H - 300, 1.2f, TXT_DIM, s_videoDiag, SCREEN_W - 128, 10);
    DrawFooter("DPAD : naviguer   |   A : ouvrir   |   X : test video   |   BACK+START : quitter");
}

static void ScreenBrowse()
{
    char head[200];
    sprintf_s(head, "%s  (%d)", s_browse.title[0] ? s_browse.title : "Bibliotheque", s_nItems);
    DrawHeader(head);

    if (s_listLoading) {
        Render::Text(64, 160, 2.f, TXT_DIM, "Chargement...");
        DrawFooter("B : retour");
        return;
    }
    if (s_nItems == 0) {
        Render::Text(64, 160, 2.f, TXT_MAIN, "Vide.");
        DrawFooter("B : retour   |   BACK+START : quitter");
        return;
    }

    int totalRows = (s_nItems + COLS - 1) / COLS;
    for (int row = 0; row < ROWS_VIS; ++row) {
        int itemRow = s_scrollRow + row;
        for (int col = 0; col < COLS; ++col) {
            int idx = itemRow * COLS + col;
            if (idx >= s_nItems) break;
            float x = (float)(GRID_X + col * CELL_W);
            float y = (float)(GRID_Y + row * ROW_H);
            bool sel = (idx == s_sel);
            if (sel) {
                Render::FillRect(x - 8, y - 8, POSTER_W + 16, POSTER_H + 56, 0xFF1E3D16);
                Render::RectOutline(x - 8, y - 8, POSTER_W + 16, POSTER_H + 56, 3, ACCENT);
            }
            if (s_thumbs[idx])
                Render::Image(s_thumbs[idx], x, y, POSTER_W, POSTER_H);
            else {
                Render::FillRect(x, y, POSTER_W, POSTER_H, 0xFF1A1A1A);
                Render::Text(x + POSTER_W/2 - 20, y + POSTER_H/2 - 8, 1.5f, 0xFF555555, "...");
            }
            DWORD tc = sel ? TXT_MAIN : TXT_DIM;
            Render::TextEllipsis(x, y + POSTER_H + 8, 1.4f, tc, s_items[idx].title, POSTER_W + 8);
            if (s_items[idx].year[0])
                Render::Text(x, y + POSTER_H + 26, 1.3f, TXT_DIM, s_items[idx].year);
        }
    }

    // indicateur de position
    Render::TextF(SCREEN_W - 200, 112, 1.5f, TXT_DIM,
                  "ligne %d/%d", s_sel / COLS + 1, totalRows);
    DrawFooter("DPAD : naviguer   |   A : ouvrir   |   LB/RB : sauts   |   B : retour");
}

static void DrawWrapped(float x, float y, float scale, DWORD color, const char* text, float maxW, int maxLines)
{
    char buf[PLEX_MAX_SUMMARY];
    strcpy_s(buf, sizeof(buf), text);
    int charsPerLine = (int)(maxW / (8.f * scale));
    int line = 0;
    char* p = buf;
    while (*p && line < maxLines) {
        int len = (int)strlen(p);
        if (len <= charsPerLine) {
            Render::Text(x, y + line * 26, scale, color, p);
            break;
        }
        int cut = charsPerLine;
        while (cut > 0 && p[cut] != ' ') --cut;
        if (cut == 0) cut = charsPerLine;
        char tmp[512];
        int cplen = cut < 500 ? cut : 500;
        memcpy(tmp, p, cplen); tmp[cplen] = 0;
        Render::Text(x, y + line * 26, scale, color, tmp);
        p += cut + (p[cut] == ' ' ? 1 : 0);
        ++line;
    }
}

static void ScreenDetail()
{
    if (s_sel >= s_nItems) { s_scr = SCR_BROWSE; return; }
    PlexItem& it = s_items[s_sel];
    DrawHeader(it.title);

    // poster a gauche (thumb grille reutilisee)
    float px = 64, py = 140, pw = 300, ph = 450;
    if (s_thumbs[s_sel]) Render::Image(s_thumbs[s_sel], px, py, pw, ph);
    else { Render::FillRect(px, py, pw, ph, 0xFF1A1A1A); }
    Render::RectOutline(px, py, pw, ph, 2, 0xFF2A5A1E);

    float tx = px + pw + 40;
    Render::Text(tx, py, 2.f, TXT_MAIN, it.title);
    Render::TextF(tx, py + 44, 1.5f, ACCENT, "%s  %s  %s",
                  it.type, it.year, it.isContainer ? "(collection)" : "");
    if (it.durationMs) {
        DWORD m = it.durationMs / 60000;
        Render::TextF(tx, py + 74, 1.5f, TXT_DIM, "Duree : %luh%02lu", m / 60, m % 60);
    }
    if (it.summary[0]) {
        Render::Text(tx, py + 110, 1.5f, ACCENT, "Resume :");
        DrawWrapped(tx, py + 140, 1.5f, TXT_MAIN, it.summary,
                    (float)(SCREEN_W - tx - 60), 12);
    }

    const char* act = "";
    if (strcmp(it.type, "photo") == 0) act = "A : voir en plein ecran";
    else if (it.isContainer)         act = "A : parcourir";
    else if ((strcmp(it.type, "movie") == 0 || strcmp(it.type, "episode") == 0) && it.partFile[0])
        act = "A : lecture (transcodage PC)";
    else if (strcmp(it.type, "movie") == 0 || strcmp(it.type, "episode") == 0)
        act = "fichier media introuvable";
    if (s_videoErr[0])
        Render::Text(64, SCREEN_H - 80, 1.5f, 0xFFFF6666, s_videoErr);
    if (s_videoDiag[0])
        DrawWrapped(64, SCREEN_H - 300, 1.2f, TXT_DIM, s_videoDiag, SCREEN_W - 128, 10);
    DrawFooter(act[0] ? act : "B : retour   |   BACK+START : quitter");
}

static void ScreenPhoto()
{
    Render::FillRect(0, 0, SCREEN_W, SCREEN_H, 0xFF000000);
    if (s_photoTex) {
        // fit en conservant le ratio
        D3DSURFACE_DESC d;
        s_photoTex->GetLevelDesc(0, &d);
        float sa = (float)d.Width / d.Height;
        float ta = (float)SCREEN_W / SCREEN_H;
        float w, h;
        if (sa > ta) { w = (float)SCREEN_W; h = w / sa; }
        else         { h = (float)SCREEN_H; w = h * sa; }
        Render::Image(s_photoTex, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h);
    } else {
        Render::Text(64, 340, 2.f, TXT_DIM,
                     s_photoLoading ? "Chargement..." : "Image indisponible");
    }
    Render::Text(40, SCREEN_H - 40, 1.5f, TXT_DIM, "B : retour");
}

// ---------------------------------------------------------------------------
// Update
// ---------------------------------------------------------------------------
static void Update()
{
    bool conn = (XInputGetState(0, &s_pad) == ERROR_SUCCESS);
    s_padOn = conn;

    if (conn) {
        WORD b = s_pad.Gamepad.wButtons;
        if ((b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START)) s_quit = true;

        switch (s_scr) {
        case SCR_NET_ERR:
            if (Pressed(XINPUT_GAMEPAD_A)) {
                s_sectionsAsked = false;
                s_scr = SCR_SECTIONS;
            }
            if (Pressed(XINPUT_GAMEPAD_X) && !s_probing) RunProbes();
            break;
        case SCR_SECTIONS:
            if (Pressed(XINPUT_GAMEPAD_DPAD_UP)   && s_secSel > 0) --s_secSel;
            if (Pressed(XINPUT_GAMEPAD_DPAD_DOWN) && s_secSel < s_nSections - 1) ++s_secSel;
            if (Pressed(XINPUT_GAMEPAD_A) && s_nSections > 0) {
                BrowseCtx c;
                c.bySection = true;
                strcpy_s(c.key, sizeof(c.key), s_sections[s_secSel].key);
                strcpy_s(c.title, sizeof(c.title), s_sections[s_secSel].title);
                PushNav(SCR_SECTIONS, NULL, s_secSel);
                RequestList(c);
            }
            if (Pressed(XINPUT_GAMEPAD_X))
                PlayVideoFile("game:\\test.wmv");
            if (Pressed(XINPUT_GAMEPAD_Y))
                PlayVideoUserIo("game:\\test.wmv");
            break;
        case SCR_BROWSE: {
            if (s_listLoading) { if (Pressed(XINPUT_GAMEPAD_B)) GoBack(); break; }
            int row = s_sel / COLS, col = s_sel % COLS;
            if (Pressed(XINPUT_GAMEPAD_DPAD_LEFT)  && col > 0)          --s_sel;
            if (Pressed(XINPUT_GAMEPAD_DPAD_RIGHT) && s_sel < s_nItems-1) ++s_sel;
            if (Pressed(XINPUT_GAMEPAD_DPAD_UP)    && s_sel >= COLS)     s_sel -= COLS;
            if (Pressed(XINPUT_GAMEPAD_DPAD_DOWN)  && s_sel + COLS < s_nItems) s_sel += COLS;
            if (Pressed(XINPUT_GAMEPAD_LEFT_SHOULDER))  s_sel = max(0, s_sel - 10);
            if (Pressed(XINPUT_GAMEPAD_RIGHT_SHOULDER)) s_sel = min(s_nItems - 1, s_sel + 10);
            // scroll auto
            int selRow = s_sel / COLS;
            if (selRow < s_scrollRow) s_scrollRow = selRow;
            if (selRow >= s_scrollRow + ROWS_VIS) s_scrollRow = selRow - ROWS_VIS + 1;
            if (Pressed(XINPUT_GAMEPAD_B)) GoBack();
            if (Pressed(XINPUT_GAMEPAD_A)) {
                PlexItem& it = s_items[s_sel];
                if (it.isContainer) {
                    BrowseCtx c;
                    c.bySection = false;
                    if (it.childPath[0]) strcpy_s(c.key, sizeof(c.key), it.childPath);
                    else Plex::PathChildren(it.ratingKey, c.key, sizeof(c.key));
                    strcpy_s(c.title, sizeof(c.title), it.title);
                    PushNav(SCR_BROWSE, &s_browse, s_sel);
                    RequestList(c);
                } else if (strcmp(it.type, "photo") == 0) {
                    // viewer photo plein ecran
                    if (s_photoTex) { s_photoTex->Release(); s_photoTex = NULL; }
                    char path[1200];
                    Plex::PathTranscodeThumb(it.thumb, SCREEN_W, SCREEN_H, path, sizeof(path));
                    s_photoLoading = true;
                    QueueJob(JOB_PHOTO, path);
                    s_scr = SCR_PHOTO;
                } else {
                    s_scr = SCR_DETAIL;
                }
            }
            break;
        }
        case SCR_DETAIL:
            if (Pressed(XINPUT_GAMEPAD_B)) { s_scr = SCR_BROWSE; }
            if (Pressed(XINPUT_GAMEPAD_A)) {
                PlexItem& it = s_items[s_sel];
                if ((strcmp(it.type, "movie") == 0 || strcmp(it.type, "episode") == 0)
                    && it.partFile[0]) {
                    PlayStreamedVideo(it);
                }
                else if (strcmp(it.type, "photo") == 0) {
                    if (s_photoTex) { s_photoTex->Release(); s_photoTex = NULL; }
                    char path[1200];
                    Plex::PathTranscodeThumb(it.thumb, SCREEN_W, SCREEN_H, path, sizeof(path));
                    s_photoLoading = true;
                    QueueJob(JOB_PHOTO, path);
                    s_scr = SCR_PHOTO;
                }
            }
            break;
        case SCR_PHOTO:
            if (Pressed(XINPUT_GAMEPAD_B)) s_scr = SCR_DETAIL;
            break;
        default: break;
        }
        s_prevButtons = b;
    }
}

// ---------------------------------------------------------------------------
void __cdecl main()
{
    InitializeCriticalSection(&s_qLock);
    memset(s_thumbs, 0, sizeof(s_thumbs));

    if (FAILED(InitD3D()) || FAILED(Render::Init(g_device))) return;

    // XAudio2 requis par le player XMV
    HRESULT hrX = XAudio2Create(&g_xaudio, 0);
    if (SUCCEEDED(hrX) && g_xaudio) {
        IXAudio2MasteringVoice* mv = NULL;
        HRESULT hrM = g_xaudio->CreateMasteringVoice(&mv); // gardee vivante pour le player XMV
        if (FAILED(hrM))
            sprintf_s(s_videoErr, sizeof(s_videoErr), "MasterVoice KO 0x%08lX", hrM);
    } else {
        sprintf_s(s_videoErr, sizeof(s_videoErr), "XAudio2Create KO 0x%08lX", hrX);
        g_xaudio = NULL;
    }

    if (!LoadConfig()) { s_scr = SCR_CONFIG_ERR; }
    else {
        DWORD rc = Net::Init();
        if (rc != 0) {
            char msg[128];
            sprintf_s(msg, sizeof(msg), "Init reseau etape %lu : %s", rc, Net::LocalIPStr());
            SetError(SCR_NET_ERR, msg);
        } else {
            s_scr = SCR_SECTIONS;
            HANDLE h = CreateThread(NULL, 0, WorkerProc, NULL, 0, NULL);
            if (h) CloseHandle(h);
        }
    }

    bool running = true;
    while (running && !s_quit) {
        DrainResults();

        // fetch des sections au boot / retry
        if (s_scr == SCR_SECTIONS && !s_sectionsLoaded && !s_sectionsAsked) {
            s_sectionsAsked = true;
            char path[64]; Plex::PathSections(path, sizeof(path));
            QueueJob(JOB_SECTIONS, path);
        }

        Update();
        RequestVisibleThumbs();

        Render::BeginFrame();
        switch (s_scr) {
            case SCR_BOOT:       ScreenBoot();      break;
            case SCR_CONFIG_ERR: ScreenConfigErr(); break;
            case SCR_NET_ERR:    ScreenNetErr();    break;
            case SCR_SECTIONS:   ScreenSections();  break;
            case SCR_BROWSE:     ScreenBrowse();    break;
            case SCR_DETAIL:     ScreenDetail();    break;
            case SCR_PHOTO:      ScreenPhoto();     break;
        }
        Render::EndFrame();
    }

    s_workerRun = false;
    ClearThumbs();
    Render::Shutdown();
    Net::Shutdown();
}
