// net.cpp — client HTTP bloquant via sockets BSD (pattern du sample HttpSocket du XDK)
#include "net.h"
#include <winsockx.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

namespace Net {

static bool s_ready = false;
static char s_localIP[24] = "";
static int  s_stage = 0;
static int  s_err   = 0;
static int  s_ioTimeoutMs = 10000;

const char* LocalIPStr() { return s_localIP; }
int LastStage() { return s_stage; }
int LastErr()   { return s_err; }
void SetIoTimeout(int ms) { s_ioTimeoutMs = ms > 0 ? ms : 10000; }

DWORD Init()
{
    XNetStartupParams xnsp;
    memset(&xnsp, 0, sizeof(xnsp));
    xnsp.cfgSizeOfStruct = sizeof(XNetStartupParams);
    xnsp.cfgFlags = XNET_STARTUP_BYPASS_SECURITY; // ignore sur retail
    DWORD dw = XNetStartup(&xnsp);
    if (dw != 0) {
        sprintf_s(s_localIP, sizeof(s_localIP), "XNetStartup=0x%08lX", dw);
        return 1;
    }

    // XNetGetTitleXnAddr retourne des FLAGS (ETHERNET/STATIC/DHCP...), pas un
    // code d'erreur : 0 = en cours, bit NONE = pas d'adresse, le reste = OK.
    XNADDR xna;
    DWORD r;
    int spins = 0;
    do {
        r = XNetGetTitleXnAddr(&xna);
        if (r == XNET_GET_XNADDR_PENDING) Sleep(100);
        if (++spins > 200) break; // ~20s max
    } while (r == XNET_GET_XNADDR_PENDING);
    if (r == 0 || (r & XNET_GET_XNADDR_NONE)) {
        sprintf_s(s_localIP, sizeof(s_localIP), "XnAddr=0x%08lX", r);
        return 2;
    }
    {
        DWORD ip = ntohl(xna.ina.s_addr);
        sprintf_s(s_localIP, sizeof(s_localIP), "%lu.%lu.%lu.%lu",
                  (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
                  (ip >> 8) & 0xFF, ip & 0xFF);
    }

    WSADATA wsa;
    dw = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (dw != 0) {
        sprintf_s(s_localIP, sizeof(s_localIP), "WSAStartup=0x%08lX", dw);
        return 3;
    }
    s_ready = true;
    return 0;
}

void Shutdown()
{
    if (s_ready) { WSACleanup(); XNetCleanup(); s_ready = false; }
}

// Sur retail, XNET_STARTUP_BYPASS_SECURITY est ignore : chaque socket reste
// "secure" (trame XNet chiffree, system link) tant qu'on ne passe pas ces
// options SOL_SOCKET non documentees. Sans ca, le PC ne comprend rien et
// connect() part en timeout.
static void MakeInsecure(SOCKET s)
{
    BOOL on = TRUE;
    setsockopt(s, SOL_SOCKET, 0x5801, (const char*)&on, sizeof(on));
    setsockopt(s, SOL_SOCKET, 0x5802, (const char*)&on, sizeof(on));
}

// Resout host (IP litterale ou DNS via XNetDnsLookup).
static bool Resolve(const char* host, sockaddr_in* out)
{
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_addr.s_addr = inet_addr(host);
    if (out->sin_addr.s_addr != INADDR_NONE) return true;

    HANDLE hEv = WSACreateEvent();
    if (!hEv) return false;
    XNDNS* dns = NULL;
    XNetDnsLookup(host, hEv, &dns);
    WaitForSingleObject(hEv, 8000);
    WSACloseEvent(hEv);
    bool ok = dns && dns->iStatus == 0 && dns->cina > 0;
    if (ok) out->sin_addr = dns->aina[0];
    if (dns) XNetDnsRelease(dns);
    return ok;
}

// Decode transfer-encoding chunked (Plex peut en envoyer).
static DWORD DecodeChunked(BYTE* data, DWORD size)
{
    DWORD rd = 0, wr = 0;
    while (rd < size) {
        // taille de chunk en hexa jusqu'a CRLF
        DWORD chunk = 0;
        while (rd < size && data[rd] != '\r') {
            char c = data[rd];
            if (c == ';') { // extension de chunk : ignore jusqu'a CRLF
                while (rd < size && data[rd] != '\r') rd++;
                break;
            }
            int d = (c >= '0' && c <= '9') ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (d < 0) { rd++; continue; }
            chunk = chunk * 16 + d; rd++;
        }
        while (rd < size && (data[rd] == '\r' || data[rd] == '\n')) rd++;
        if (chunk == 0) break;
        if (rd + chunk > size) chunk = size - rd;
        memmove(data + wr, data + rd, chunk);
        wr += chunk; rd += chunk;
        while (rd < size && (data[rd] == '\r' || data[rd] == '\n')) rd++;
    }
    return wr;
}

static bool FinishResponse(BYTE* buf, DWORD len,
                           BYTE** outData, DWORD* outSize, DWORD* outStatus);

static bool FindHeaderEnd(BYTE* buf, DWORD size, DWORD* hdrEnd)
{
    for (DWORD i = 0; i + 3 < size; ++i)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n') {
            *hdrEnd = i + 4;
            return true;
        }
    return false;
}

bool HttpGet(const char* host, WORD port, const char* path,
             const char* extraHeaders,
             BYTE** outData, DWORD* outSize, DWORD* outStatus)
{
    *outData = NULL; *outSize = 0; *outStatus = 0;
    s_stage = 0; s_err = 0;
    if (!s_ready) return false;

    sockaddr_in addr;
    if (!Resolve(host, &addr)) { s_stage = 1; s_err = WSAGetLastError(); return false; }
    addr.sin_port = htons(port);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { s_stage = 2; s_err = WSAGetLastError(); return false; }
    MakeInsecure(s);

    int tv = s_ioTimeoutMs;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        s_stage = 3; s_err = WSAGetLastError();
        closesocket(s);
        return false;
    }

    char req[2048];
    int reqLen = sprintf_s(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: Plex360/1.0\r\n"
        "Accept: */*\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        path, host, port, extraHeaders ? extraHeaders : "");
    if (send(s, req, reqLen, 0) == SOCKET_ERROR) {
        s_stage = 4; s_err = WSAGetLastError();
        closesocket(s);
        return false;
    }

    // Recoit tout jusqu'a fermeture (Connection: close)
    DWORD cap = 1 << 20, len = 0;
    BYTE* buf = (BYTE*)malloc(cap);
    if (!buf) { closesocket(s); return false; }
    for (;;) {
        if (len + 65536 > cap) {
            cap *= 2;
            BYTE* nb = (BYTE*)realloc(buf, cap);
            if (!nb) { free(buf); closesocket(s); return false; }
            buf = nb;
        }
        int n = recv(s, (char*)buf + len, 65536, 0);
        if (n <= 0) break;
        len += n;
    }
    if (len == 0) s_err = WSAGetLastError(); // timeout/reset sur 1er recv
    shutdown(s, SD_BOTH);
    closesocket(s);
    return FinishResponse(buf, len, outData, outSize, outStatus);
}

// Parse status + headers, extrait le body (chunked inclus) dans outData.
// buf est consomme/libere dans tous les cas de retour.
static bool FinishResponse(BYTE* buf, DWORD len,
                           BYTE** outData, DWORD* outSize, DWORD* outStatus)
{
    if (len < 12) { s_stage = 5; free(buf); return false; }

    // --- Parse headers ---
    DWORD hdrEnd;
    if (!FindHeaderEnd(buf, len, &hdrEnd)) { s_stage = 6; free(buf); return false; }
    // status code
    char* sp = strchr((char*)buf, ' ');
    if (!sp) { s_stage = 7; free(buf); return false; }
    *outStatus = atoi(sp + 1);

    // content-length / chunked
    DWORD contentLength = 0;
    bool chunked = false;
    {
        // scan ligne par ligne dans les headers
        DWORD pos = (DWORD)(sp - (char*)buf);
        while (pos < hdrEnd) {
            char* ls = (char*)buf + pos;
            char* le = strstr(ls, "\r\n");
            if (!le || (DWORD)(le - (char*)buf) >= hdrEnd) break;
            *le = 0;
            if (_strnicmp(ls, "Content-Length:", 15) == 0)
                contentLength = atoi(ls + 15);
            else if (_strnicmp(ls, "Transfer-Encoding:", 18) == 0)
                chunked = strstr(ls + 18, "chunked") != NULL;
            pos = (DWORD)(le - (char*)buf) + 2;
        }
    }

    DWORD bodyLen = len - hdrEnd;
    BYTE* body = buf + hdrEnd;
    if (chunked)
        bodyLen = DecodeChunked(body, bodyLen);
    else if (contentLength && contentLength < bodyLen)
        bodyLen = contentLength;

    // compacte : copie le body au debut puis realloc
    memmove(buf, body, bodyLen);
    BYTE* shrunk = (BYTE*)realloc(buf, bodyLen + 1);
    if (shrunk) buf = shrunk;
    buf[bodyLen] = 0;

    *outData = buf;
    *outSize = bodyLen;
    return *outStatus >= 200 && *outStatus < 300;
}

// ---- HTTPS (TLS 1.2 via XboxTLS/BearSSL) ----
// Anchors : ISRG Root X1 (Let's Encrypt) + GTS Root R4 (Google Trust) —
// couvrent l'essentiel du web moderne, dont plex.tv (Cloudflare/GTS/LE).
#include "tls/XboxTLS.h"

static const unsigned char TA_ISRG_DN[] = {
    0x30,0x31,0x31,0x0B,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,
    0x31,0x13,0x30,0x11,0x06,0x03,0x55,0x04,0x0A,0x13,0x0A,0x49,0x53,0x52,0x47,
    0x20,0x2C,0x49,0x6E,0x63,0x2E,0x31,0x13,0x30,0x11,0x06,0x03,0x55,0x04,0x03,
    0x13,0x0A,0x49,0x53,0x52,0x47,0x20,0x52,0x6F,0x6F,0x74,0x20,0x58,0x31
};
static const unsigned char TA_ISRG_N[] = {
    0x00, 0xaf, 0x2f, 0x62, 0xe9, 0xf5, 0x3d, 0x1f, 0x64, 0x2e, 0x98, 0x0f, 0x09, 0x3a, 0x65, 0x9b,
    0xf5, 0x77, 0x6f, 0x47, 0xdc, 0x96, 0xf9, 0x4e, 0x58, 0x91, 0x1f, 0x94, 0xb6, 0x1b, 0x7f, 0x7d,
    0x25, 0xa4, 0x0c, 0xc2, 0x55, 0x43, 0xd6, 0x62, 0xe3, 0xf3, 0x82, 0xc5, 0x0b, 0x12, 0x4d, 0xb0,
    0x0e, 0xb3, 0x4c, 0x4e, 0xf0, 0xac, 0x6a, 0x26, 0x4e, 0xd3, 0x93, 0xf4, 0x39, 0xd2, 0xc8, 0x2c,
    0x3b, 0xc6, 0x0a, 0xc7, 0x57, 0x18, 0x6c, 0xd1, 0x60, 0x60, 0x87, 0xd8, 0xac, 0x00, 0x11, 0x5d,
    0xb3, 0x69, 0x6a, 0x25, 0x80, 0xa5, 0x6f, 0x84, 0x2c, 0x1b, 0x33, 0x61, 0x4a, 0xe7, 0xd1, 0x8d,
    0x1f, 0xa2, 0xb0, 0x0d, 0x2d, 0xea, 0xbb, 0x0e, 0x5f, 0xe2, 0x7f, 0xa5, 0x80, 0xd2, 0x5f, 0xb7,
    0x25, 0x34, 0xb0, 0x4e, 0x76, 0x9e, 0x2c, 0x83, 0x25, 0xb2, 0x3e, 0x33, 0xe7, 0x2d, 0x5e, 0x45,
    0x93, 0xa4, 0xb2, 0x2b, 0x73, 0x1a, 0x6c, 0xf4, 0x30, 0x95, 0x28, 0x3b, 0x6b, 0xa3, 0x75, 0x4d,
    0x38, 0xbe, 0x7a, 0x11, 0x3c, 0xdf, 0x71, 0x33, 0x4f, 0x0e, 0x9e, 0x6d, 0xe5, 0xa6, 0x76, 0x7e,
    0x3e, 0xf6, 0xf4, 0x91, 0x8a, 0xbe, 0x3d, 0xf4, 0x11, 0xc4, 0x91, 0x0a, 0xe3, 0x5c, 0x2f, 0xbe,
    0x2e, 0x27, 0x3e, 0x61, 0x61, 0xb4, 0x12, 0xfa, 0xb9, 0xd4, 0x26, 0x44, 0xbd, 0x1a, 0xd3, 0x12,
    0x68, 0x96, 0xa2, 0x92, 0x7a, 0x8b, 0x86, 0x4d, 0x12, 0x29, 0xa1, 0x77, 0x53, 0x4a, 0x9a, 0x35,
    0xe2, 0xa1, 0x56, 0x45, 0xc5, 0xf3, 0xd7, 0x70, 0xd7, 0x91, 0x9f, 0x8c, 0x1b, 0xdf, 0x1c, 0x0b,
    0xb1, 0x3d, 0xa7, 0xf2, 0xbb, 0xd9, 0x6b, 0x75, 0x8d, 0x2d, 0x7b, 0xc7, 0x19, 0x5b, 0x9f, 0x32,
    0xbc, 0x3a, 0x1a, 0xd5, 0xa3, 0x93, 0xb3, 0xf9, 0x75, 0x26, 0x2e, 0x67, 0xf2, 0x77, 0x93, 0x41
};
static const unsigned char TA_ISRG_E[] = { 0x01, 0x00, 0x01 };

static const unsigned char TA_GTS_DN[] = {
    0x30,0x47,0x31,0x0B,0x30,0x09,0x06,0x03,0x55,0x04,0x06,0x13,0x02,0x55,0x53,
    0x31,0x22,0x30,0x20,0x06,0x03,0x55,0x04,0x0A,0x13,0x19,0x47,0x6F,0x6F,0x67,
    0x6C,0x65,0x20,0x54,0x72,0x75,0x73,0x74,0x20,0x53,0x65,0x72,0x76,0x69,0x63,
    0x65,0x73,0x20,0x4C,0x4C,0x43,0x31,0x14,0x30,0x12,0x06,0x03,0x55,0x04,0x03,
    0x13,0x0B,0x47,0x54,0x53,0x20,0x52,0x6F,0x6F,0x74,0x20,0x52,0x34
};
static const unsigned char TA_GTS_Q[] = {
    0x04,0xF3,0x74,0x73,0xA7,0x68,0x8B,0x60,0xAE,0x43,0xB8,0x35,0xC5,0x81,0x30,
    0x7B,0x4B,0x49,0x9D,0xFB,0xC1,0x61,0xCE,0xE6,0xDE,0x46,0xBD,0x6B,0xD5,0x61,
    0x18,0x35,0xAE,0x40,0xDD,0x73,0xF7,0x89,0x91,0x30,0x5A,0xEB,0x3C,0xEE,0x85,
    0x7C,0xA2,0x40,0x76,0x3B,0xA9,0xC6,0xB8,0x47,0xD8,0x2A,0xE7,0x92,0x91,0x6A,
    0x73,0xE9,0xB1,0x72,0x39,0x9F,0x29,0x9F,0xA2,0x98,0xD3,0x5F,0x5E,0x58,0x86,
    0x65,0x0F,0xA1,0x84,0x65,0x06,0xD1,0xDC,0x8B,0xC9,0xC7,0x73,0xC8,0x8C,0x6A,
    0x2F,0xE5,0xC4,0xAB,0xD1,0x1D,0x8A
};

// Meme usage que HttpGet mais en HTTPS (TLS 1.2). port conseille : 443.
bool HttpsGet(const char* host, WORD port, const char* path,
              const char* extraHeaders,
              BYTE** outData, DWORD* outSize, DWORD* outStatus)
{
    *outData = NULL; *outSize = 0; *outStatus = 0;
    s_stage = 0; s_err = 0;
    if (!s_ready) return false;

    sockaddr_in addr;
    if (!Resolve(host, &addr)) { s_stage = 1; s_err = WSAGetLastError(); return false; }
    char ip[24];
    sprintf_s(ip, sizeof(ip), "%lu.%lu.%lu.%lu",
              (DWORD)addr.sin_addr.S_un.S_un_b.s_b1,
              (DWORD)addr.sin_addr.S_un.S_un_b.s_b2,
              (DWORD)addr.sin_addr.S_un.S_un_b.s_b3,
              (DWORD)addr.sin_addr.S_un.S_un_b.s_b4);

    XboxTLSContext ctx;
    if (!XboxTLS_CreateContext(&ctx, host)) { s_stage = 2; return false; }
    ctx.hashAlgo = XboxTLS_Hash_SHA384;
    XboxTLS_AddTrustAnchor_RSA(&ctx, TA_ISRG_DN, sizeof(TA_ISRG_DN),
                               TA_ISRG_N, sizeof(TA_ISRG_N),
                               TA_ISRG_E, sizeof(TA_ISRG_E));
    XboxTLS_AddTrustAnchor_EC(&ctx, TA_GTS_DN, sizeof(TA_GTS_DN),
                              TA_GTS_Q, sizeof(TA_GTS_Q), XboxTLS_Curve_secp384r1);

    if (!XboxTLS_Connect(&ctx, ip, host, port)) {
        s_stage = 3; s_err = -1;
        XboxTLS_Free(&ctx);
        return false;
    }

    char req[2048];
    int reqLen = sprintf_s(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: Plex360/1.0\r\n"
        "Accept: */*\r\n"
        "%s"
        "Connection: close\r\n\r\n",
        path, host, extraHeaders ? extraHeaders : "");
    if (XboxTLS_Write(&ctx, req, reqLen) < 0) {
        s_stage = 4;
        XboxTLS_Free(&ctx);
        return false;
    }

    DWORD cap = 1 << 20, len = 0;
    BYTE* buf = (BYTE*)malloc(cap);
    if (!buf) { XboxTLS_Free(&ctx); return false; }
    for (;;) {
        if (len + 65536 > cap) {
            cap *= 2;
            BYTE* nb = (BYTE*)realloc(buf, cap);
            if (!nb) { free(buf); XboxTLS_Free(&ctx); return false; }
            buf = nb;
        }
        int n = XboxTLS_Read(&ctx, buf + len, 65536);
        if (n <= 0) break;
        len += n;
    }
    XboxTLS_Free(&ctx);
    return FinishResponse(buf, len, outData, outSize, outStatus);
}

// Connect TCP non bloquant + select : distingue refuse/timeout/OK.
bool Probe(const char* ip, WORD port, int timeoutMs)
{
    if (!s_ready) { s_stage = 0; return false; }
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    if (addr.sin_addr.s_addr == INADDR_NONE) { s_stage = 1; return false; }
    addr.sin_port = htons(port);

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { s_stage = 2; s_err = WSAGetLastError(); return false; }
    MakeInsecure(s);

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr*)&addr, sizeof(addr)); // WSAEWOULDBLOCK attendu

    fd_set w, e;
    FD_ZERO(&w); FD_ZERO(&e);
    FD_SET(s, &w); FD_SET(s, &e);
    timeval t; t.tv_sec = timeoutMs / 1000; t.tv_usec = (timeoutMs % 1000) * 1000;
    int r = select(0, NULL, &w, &e, &t);
    bool ok = false;
    if (r > 0) {
        if (FD_ISSET(s, &e)) {
            // echec de connexion : un 2e connect() renvoie le code reel
            connect(s, (sockaddr*)&addr, sizeof(addr));
            s_err = WSAGetLastError();
            s_stage = 3;
        } else if (FD_ISSET(s, &w)) {
            ok = true; s_err = 0; s_stage = 0;
        }
    } else {
        s_stage = 3; s_err = 10060; // timeout select
    }
    closesocket(s);
    return ok;
}

} // namespace Net
