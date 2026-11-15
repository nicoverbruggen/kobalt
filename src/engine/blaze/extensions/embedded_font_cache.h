// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_EMBEDDED_FONT_CACHE_H
#define BLAZE_EMBEDDED_FONT_CACHE_H

#include "lvstream.h"
#include <string>
#include <unordered_map>

namespace blaze {

// Chapter documents of the same book can share embedded font bytes.
class EmbeddedFontCache {
    std::unordered_map<std::string, LVByteArrayRef> _entries;
    size_t _bytes = 0;
public:
    LVByteArrayRef read(const lString8 & name, const LVStreamRef & stream, lvsize_t size);
    void clear();
};

}

#endif
