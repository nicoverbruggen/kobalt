// SPDX-License-Identifier: GPL-2.0-or-later
#include "css_class_index.h"
#include "lvstsheet.h"
#include <algorithm>
#include <unordered_set>

void LVCssClassIndex::rebuild(const LVCssSelector *first)
{
    _selectors.clear();
    _classes.clear();
    _fallback.clear();
    // A sheet without indexable classes can use its original chain directly.
    lUInt32 hash;
    auto *candidate = first;
    while (candidate && !candidate->requiredClassHash(hash))
        candidate = candidate->getNext();
    if (!candidate)
        return;
    for (auto *s = first; s; s = s->getNext()) {
        auto &positions = s->requiredClassHash(hash) ? _classes[hash] : _fallback;
        positions.push_back(_selectors.size());
        _selectors.push_back(s);
    }
}

LVCssClassIndex::Cursor::Cursor(const LVCssClassIndex &index, const lUInt32 *classes, size_t count)
    : _index(index)
{
    std::unordered_set<lUInt32> combinedClasses;
    for (size_t i = 0; i < count; ++i) {
        auto it = index._classes.find(classes[i]);
        if (it == index._classes.end() || _matches == &it->second)
            continue;
        if (!_matches) {
            _matches = &it->second;
            continue;
        }
        if (!combinedClasses.insert(classes[i]).second)
            continue;
        if (_combined.empty())
            _combined = *_matches;
        _combined.insert(_combined.end(), it->second.begin(), it->second.end());
    }
    if (!_combined.empty()) {
        std::sort(_combined.begin(), _combined.end());
        _matches = &_combined;
    }
}

const LVCssSelector *LVCssClassIndex::Cursor::next()
{
    auto &fallback = _index._fallback;
    if (_matches && _match < _matches->size()
        && (_other == fallback.size() || (*_matches)[_match] < fallback[_other]))
        return _index._selectors[(*_matches)[_match++]];
    if (_other < fallback.size())
        return _index._selectors[fallback[_other++]];
    return nullptr;
}
