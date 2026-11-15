// Class-name splitting comes from CoolReader Engine lvstsheet.cpp.
// (c) Vadim Lopatin, 2000-2006. Distributed under the GNU General Public License.
#include "css_ancestors.h"
#include "lvstsheet.h"
#include "lvtinydom.h"
#include "fb2def.h"

void appendCssClassHashes( const lString32 & value, LVArray<lUInt32> & hashes )
{
    const lChar32 *begin = value.c_str();
    const lChar32 *end = begin;
    while (*end) {
        if (*end == ' ') {
            if (end > begin)
                hashes.add(lString32::getHash(begin, end));
            begin = end + 1;
        }
        ++end;
    }
    if (end > begin)
        hashes.add(lString32::getHash(begin, end));
}

void LVCssSelector::initAncestorRequirements()
{
    _ancestorIds.clear();
    _ancestorClasses.clear();
    // Only the combinators move the checked node: a child or descendant
    // combinator to an ancestor, a sibling combinator to a sibling. The other
    // rules check the node that the last combinator reached.
    bool ancestor = false;
    for ( const LVCssSelectorRule * rule = _rules.get(); rule; rule = rule->getNext() ) {
        switch ( rule->getType() ) {
        case cssrt_parent:
        case cssrt_ancessor:
            ancestor = true;
            if ( rule->getId() )
                _ancestorIds.push_back( rule->getId() );
            break;
        case cssrt_predecessor:
        case cssrt_predsibling:
            ancestor = false;
            break;
        case cssrt_class:
            if ( ancestor )
                _ancestorClasses.push_back( rule->getValueHash() );
            break;
        default:
            break;
        }
    }
}

bool LVCssAncestors::hasId( lUInt16 id ) {
    if ( !_idsRead ) {
        _idsRead = true;
        for ( const ldomNode * n = _node->getParentNode(); n; n = n->getParentNode() )
            _ids.add( n->getNodeId() );
    }
    for ( int i = 0; i < _ids.length(); i++ ) {
        if ( _ids[i] == id )
            return true;
    }
    return false;
}

bool LVCssAncestors::hasClass( lUInt32 hash ) {
    if ( !_classesRead ) {
        _classesRead = true;
        // The same attribute and the same split as the class rules and
        // LVStyleSheet::apply() use.
        for ( const ldomNode * n = _node->getParentNode(); n; n = n->getParentNode() ) {
            if ( !n->isElement() )
                continue;
            const lString32 & v = n->getAttributeValue(attr_class);
            appendCssClassHashes(v, _classes);
        }
    }
    for ( int i = 0; i < _classes.length(); i++ ) {
        if ( _classes[i] == hash )
            return true;
    }
    return false;
}
