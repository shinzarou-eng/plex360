// plex.cpp — Plex Media Server : chemins API + parsing XML
#include "plex.h"
#include "xmlmini.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

namespace Plex {

static char s_srvHost[64] = "";
static WORD s_srvPort = 32400;
static bool s_useTls = false;
static char s_token[128] = "";
static char s_headers[256] = "";

void SetServer(const char* host, WORD port, const char* token)
{
    strcpy_s(s_srvHost, sizeof(s_srvHost), host ? host : "");
    s_srvPort = port ? port : 32400;
    strcpy_s(s_token, sizeof(s_token), token ? token : "");
    if (s_token[0])
        sprintf_s(s_headers, sizeof(s_headers), "X-Plex-Token: %s\r\n", s_token);
    else
        s_headers[0] = 0;
}

void SetUseTls(bool on) { s_useTls = on; }
bool UseTls() { return s_useTls; }

const char* Host() { return s_srvHost; }
WORD Port() { return s_srvPort; }
const char* ExtraHeaders() { return s_headers; }

void PathSections(char* out, int cap)
{
    strcpy_s(out, cap, "/library/sections");
}

void PathSectionItems(const char* key, char* out, int cap)
{
    sprintf_s(out, cap, "/library/sections/%s/all", key);
}

void PathChildren(const char* ratingKey, char* out, int cap)
{
    sprintf_s(out, cap, "/library/metadata/%s/children", ratingKey);
}

// Percent-encode (tout sauf alnum et -._~)
void UrlEncode(const char* in, char* out, int cap)
{
    int o = 0;
    for (const char* p = in; *p && o < cap - 4; ++p) {
        unsigned char c = (unsigned char)*p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') {
            out[o++] = (char)c;
        } else {
            o += sprintf_s(out + o, cap - o, "%%%02X", c);
        }
    }
    out[o] = 0;
}

void PathTranscodeThumb(const char* thumb, int w, int h, char* out, int cap)
{
    char enc[512];
    UrlEncode(thumb, enc, sizeof(enc));
    sprintf_s(out, cap,
        "/photo/:/transcode?width=%d&height=%d&minSize=1&upscale=1&url=%s",
        w, h, enc);
}

static void CopyAttr(char* dst, int cap, const XmlEl& el, const char* name)
{
    const char* v = el.attr(name);
    strcpy_s(dst, cap, v ? v : "");
}

int ParseSections(const char* xml, PlexSection* out, int maxOut)
{
    std::vector<XmlEl> els = XmlFindAll(xml, "Directory");
    int n = 0;
    for (size_t i = 0; i < els.size() && n < maxOut; ++i) {
        const XmlEl& e = els[i];
        CopyAttr(out[n].key,   sizeof(out[n].key),   e, "key");
        CopyAttr(out[n].title, sizeof(out[n].title), e, "title");
        CopyAttr(out[n].type,  sizeof(out[n].type),  e, "type");
        if (out[n].key[0] && out[n].title[0]) ++n;
    }
    return n;
}

int ParseItems(const char* xml, PlexItem* out, int maxOut)
{
    // Elements dans l'ordre du document : Part est enfant de Video/Media,
    // on l'attache au dernier item vu.
    struct TagEl { char tag[8]; XmlEl el; };
    std::vector<TagEl> all;
    const char* tags[] = { "Video", "Directory", "Track", "Photo", "Part" };
    for (int t = 0; t < 5; ++t) {
        std::vector<XmlEl> els = XmlFindAll(xml, tags[t]);
        for (size_t i = 0; i < els.size(); ++i) {
            TagEl te; strcpy_s(te.tag, sizeof(te.tag), tags[t]); te.el = els[i];
            all.push_back(te);
        }
    }
    // tri par position document
    for (size_t i = 1; i < all.size(); ++i) {
        TagEl tmp = all[i];
        size_t j = i;
        while (j > 0 && all[j-1].el.pos > tmp.el.pos) { all[j] = all[j-1]; --j; }
        all[j] = tmp;
    }

    int n = 0;
    for (size_t i = 0; i < all.size() && n < maxOut; ++i) {
        const XmlEl& e = all[i].el;
        if (strcmp(all[i].tag, "Part") == 0) {
            if (n > 0 && !out[n-1].partKey[0]) {
                CopyAttr(out[n-1].partKey,  sizeof(out[n-1].partKey),  e, "key");
                CopyAttr(out[n-1].partFile, sizeof(out[n-1].partFile), e, "file");
            }
            continue;
        }
        CopyAttr(out[n].ratingKey, sizeof(out[n].ratingKey), e, "ratingKey");
        // Les Directory ont "key" = chemin direct des enfants
        const char* k = e.attr("key");
        out[n].childPath[0] = 0;
        out[n].partKey[0]   = 0;
        out[n].partFile[0]  = 0;
        if (k && k[0] == '/')
            strcpy_s(out[n].childPath, sizeof(out[n].childPath), k);
        if (!out[n].ratingKey[0] && k && k[0] != '/')
            strcpy_s(out[n].ratingKey, sizeof(out[n].ratingKey), k);
        CopyAttr(out[n].title,   sizeof(out[n].title),   e, "title");
        CopyAttr(out[n].thumb,   sizeof(out[n].thumb),   e, "thumb");
        CopyAttr(out[n].type,    sizeof(out[n].type),    e, "type");
        CopyAttr(out[n].year,    sizeof(out[n].year),    e, "year");
        CopyAttr(out[n].summary, sizeof(out[n].summary), e, "summary");
        const char* d = e.attr("duration");
        out[n].durationMs = d ? (DWORD)atoi(d) : 0;
        out[n].isContainer = (strcmp(out[n].type, "show") == 0 ||
                              strcmp(out[n].type, "season") == 0 ||
                              strcmp(out[n].type, "artist") == 0 ||
                              strcmp(out[n].type, "album") == 0);
        if (out[n].title[0]) ++n;
    }
    return n;
}

} // namespace Plex
