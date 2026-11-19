// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_CSS_CLASS_INDEX_H
#define BLAZE_CSS_CLASS_INDEX_H

#include "lvtypes.h"
#include <unordered_map>
#include <vector>

class LVCssSelector;

// Positions are in cascade order in the sheet's universal selector chain.
class LVCssClassIndex {
    using Positions = std::vector<size_t>;
    std::vector<const LVCssSelector *> _selectors;
    Positions _fallback;
    std::unordered_map<lUInt32, Positions> _classes;

public:
    void rebuild(const LVCssSelector *first);
    bool hasClasses() const { return !_classes.empty(); }

    class Cursor {
        const LVCssClassIndex &_index;
        const Positions *_matches = nullptr;
        Positions _combined;
        size_t _match = 0;
        size_t _other = 0;

    public:
        Cursor(const LVCssClassIndex &index, const lUInt32 *classes, size_t count);
        Cursor(const Cursor &) = delete;
        Cursor &operator=(const Cursor &) = delete;
        const LVCssSelector *next();
    };
};

#endif
