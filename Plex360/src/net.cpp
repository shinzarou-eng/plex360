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
