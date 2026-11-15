// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef BLAZE_CSS_ANCESTORS_H
#define BLAZE_CSS_ANCESTORS_H

#include "lvarray.h"
#include "lvstring.h"

struct ldomNode;

void appendCssClassHashes( const lString32 & value, LVArray<lUInt32> & hashes );

// Compute ancestor names and classes only when a selector requires them.
class LVCssAncestors
{
    const ldomNode * _node;
    bool _idsRead = false;
    bool _classesRead = false;
    LVArray<lUInt16> _ids;
    LVArray<lUInt32> _classes;
public:
    explicit LVCssAncestors( const ldomNode * node ) : _node(node) { }
    bool hasId( lUInt16 id );
    bool hasClass( lUInt32 hash );
};

#endif
