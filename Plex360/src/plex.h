// plex.h — API Plex Media Server : builders de chemins + parseurs XML.
// Aucun appel reseau ici : le main fetch via la job queue puis parse.
#pragma once
#include <xtl.h>

#define PLEX_MAX_TITLE   160
#define PLEX_MAX_PATH    256
#define PLEX_MAX_SUMMARY 1200
#define PLEX_MAX_ITEMS   512

struct PlexSection {
    char key[16];
    char title[64];
    char type[16];   // movie | show | artist | photo
};

struct PlexItem {
    char ratingKey[32];
    char title[PLEX_MAX_TITLE];
    char thumb[PLEX_MAX_PATH];      // chemin thumb Plex (a transcoder)
    char type[16];                  // movie | show | season | episode | artist | album | track | photo
    char year[8];
    char summary[PLEX_MAX_SUMMARY];
    char childPath[PLEX_MAX_PATH];  // Directory : "key" = chemin direct des enfants
    char partKey[PLEX_MAX_PATH];    // Video : Part.key = chemin HTTP du fichier
    char partFile[PLEX_MAX_PATH];   // Video : Part.file = chemin local serveur
    DWORD durationMs;
    BOOL isContainer;               // Directory -> navigable (enfants)
};

namespace Plex {

void SetServer(const char* host, WORD port, const char* token);
const char* Host();
WORD Port();

// Headers supplementaires a passer a Net::HttpGet (token si defini).
const char* ExtraHeaders();   // "X-Plex-Token: ...\r\n" ou ""

void UrlEncode(const char* in, char* out, int cap);

// --- Chemins ---
void PathSections(char* out, int cap);                          // /library/sections
void PathSectionItems(const char* key, char* out, int cap);     // /library/sections/<key>/all
void PathChildren(const char* ratingKey, char* out, int cap);   // /library/metadata/<key>/children
void PathTranscodeThumb(const char* thumb, int w, int h, char* out, int cap);

// --- Parse XML -> structures ---
int ParseSections(const char* xml, PlexSection* out, int maxOut);
int ParseItems(const char* xml, PlexItem* out, int maxOut);

}
