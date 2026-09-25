// xmlmini.cpp — parseur d'attributs XML minimal (suffisant pour les reponses Plex)
#include "xmlmini.h"
#include <string.h>
#include <stdlib.h>

const char* XmlEl::attr(const char* name) const
{
    for (size_t i = 0; i < attrs.size(); ++i)
        if (attrs[i].first == name) return attrs[i].second.c_str();
    return NULL;
}

static void XmlDecodeEntities(std::string& s)
{
    // Entites numeriques &#NNN; / &#xHH; -> UTF-8
    size_t pos = 0;
    while ((pos = s.find("&#", pos)) != std::string::npos) {
        size_t end = s.find(';', pos + 2);
        if (end == std::string::npos) break;
        unsigned long cp = 0;
        if (s[pos + 2] == 'x' || s[pos + 2] == 'X')
            cp = strtoul(s.c_str() + pos + 3, NULL, 16);
        else
            cp = strtoul(s.c_str() + pos + 2, NULL, 10);
        char utf8[4]; int n = 0;
        if (cp < 0x80)      utf8[n++] = (char)cp;
        else if (cp < 0x800) {
            utf8[n++] = (char)(0xC0 | (cp >> 6)); utf8[n++] = (char)(0x80 | (cp & 63));
        } else if (cp < 0x10000) {
            utf8[n++] = (char)(0xE0 | (cp >> 12)); utf8[n++] = (char)(0x80 | ((cp >> 6) & 63));
            utf8[n++] = (char)(0x80 | (cp & 63));
        } else {
            utf8[n++] = (char)(0xF0 | (cp >> 18)); utf8[n++] = (char)(0x80 | ((cp >> 12) & 63));
            utf8[n++] = (char)(0x80 | ((cp >> 6) & 63)); utf8[n++] = (char)(0x80 | (cp & 63));
        }
        s.replace(pos, end - pos + 1, utf8, n);
        pos += n;
    }
    // Entites nommees XML
    struct { const char* from; const char* to; } ents[] = {
        { "&quot;", "\"" }, { "&apos;", "'" }, { "&lt;", "<" },
        { "&gt;", ">" },    { "&amp;", "&" }, // &amp; en dernier
    };
    for (size_t e = 0; e < sizeof(ents)/sizeof(ents[0]); ++e) {
        pos = 0;
        while ((pos = s.find(ents[e].from, pos)) != std::string::npos) {
            s.replace(pos, strlen(ents[e].from), ents[e].to);
            pos += strlen(ents[e].to);
        }
    }
}

std::vector<XmlEl> XmlFindAll(const char* xml, const char* tag)
{
    std::vector<XmlEl> out;
    size_t tagLen = strlen(tag);
    const char* p = xml;
    while ((p = strchr(p, '<')) != NULL) {
        ++p;
        if (*p == '/' || *p == '!' || *p == '?') continue;
        if (strncmp(p, tag, tagLen) != 0) continue;
        char next = p[tagLen];
        if (next != ' ' && next != '\t' && next != '/' && next != '>') continue;
        p += tagLen;

        XmlEl el;
        el.pos = (size_t)(p - xml);
        // attributs jusqu'a '>' ou '/>'
        while (*p && *p != '>') {
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
            if (*p == '/' || *p == '>') break;
            const char* ns = p;
            while (*p && *p != '=' && *p != ' ' && *p != '\t' && *p != '>') ++p;
            std::string name(ns, p - ns);
            while (*p == ' ' || *p == '\t') ++p;
            if (*p != '=') continue;
            ++p;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '"' || *p == '\'') {
                char q = *p++;
                const char* vs = p;
                while (*p && *p != q) ++p;
                std::string val(vs, p - vs);
                XmlDecodeEntities(val);
                el.attrs.push_back(std::make_pair(name, val));
                if (*p) ++p;
            }
        }
        out.push_back(el);
        if (*p == '>') ++p;
    }
    return out;
}
