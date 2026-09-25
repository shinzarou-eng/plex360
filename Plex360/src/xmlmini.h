// xmlmini.h — extracteur minimal d'elements XML (format Plex : tags + attributs)
#pragma once
#include <xtl.h>
#include <vector>
#include <string>

struct XmlEl {
    size_t pos;       // offset dans le document (ordre)
    std::vector<std::pair<std::string, std::string>> attrs;
    const char* attr(const char* name) const;   // NULL si absent
};

// Retourne tous les elements <tag ...> du document (self-closing ou non).
std::vector<XmlEl> XmlFindAll(const char* xml, const char* tag);
