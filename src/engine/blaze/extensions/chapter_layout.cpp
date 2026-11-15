// SPDX-License-Identifier: GPL-2.0-or-later
#include "lvdocview.h"

#if BUILD_LITE!=1
#include "lvtinydom.h"
#include "fb2def.h"
#include "lvpagesplitter.h"

lvdom_element_render_method ldomDocument::koblazeSourceMethod( ldomNode * node, lvdom_element_render_method method ) const
{
    if ( method == erm_invisible && _koblazeChangedMethodsKnown ) {
        const auto found = _koblazeChangedMethods.find( node->getDataIndex() );
        if ( found != _koblazeChangedMethods.end() )
            return (lvdom_element_render_method)found->second;
    }
    return method;
}

void ldomDocument::koblazeChangeRendMethod( ldomNode * node, lvdom_element_render_method method )
{
    _koblazeChangedMethods.emplace( node->getDataIndex(), (lUInt8)node->getRendMethod() );
    node->setRendMethod( method );
}

bool ldomDocument::koblazeRelayout()
{
    if ( !_koblazeChangedMethodsKnown )
        return false;
    for ( const auto & change : _koblazeChangedMethods )
        getTinyNode( change.first )->setRendMethod( (lvdom_element_render_method)change.second );
    _koblazeChangedMethods.clear();
    // A full render clears these with the styles.
    _renderedBlockCache.clear();
    resetNodeNumberingProps();
    _koblazeRelayout = true;
    return true;
}

bool ldomDocument::koblazeSaveLayout()
{
    _koblazeSavedLayout.reset();
    if ( !_rendered || _partial_rerendering_enabled || _pagesData.error() )
        return false;
    std::unique_ptr<KoblazeSavedLayout> saved( new KoblazeSavedLayout(_pagesData.size()) );
    saved->rects.resize(_elemCount + 1);
    saved->methods.resize(_elemCount + 1);
    for ( int i = 0; i <= _elemCount; ++i ) {
        ldomNode * node = &_elemList[i >> TNC_PART_SHIFT][i & TNC_PART_MASK];
        if ( !node->isElement() )
            continue;
        node->getRenderData(saved->rects[i]);
        saved->methods[i] = (lUInt8)node->getRendMethod();
    }
    saved->pages << _pagesData;
    if ( saved->pages.error() )
        return false;
    saved->header = _hdr;
    saved->elements = _elemCount;
    saved->texts = _textCount;
    saved->sliceFirst = _koblazeSliceFirst;
    saved->sliceLast = _koblazeSliceLast;
    saved->spineItemChars = _koblazeSpineItemChars;
    saved->changedMethods = _koblazeChangedMethods;
    saved->changedMethodsKnown = _koblazeChangedMethodsKnown;
    _koblazeSavedLayout = std::move(saved);
    return true;
}

bool ldomDocument::koblazeRestoreLayout( LVRendPageList * pages )
{
    std::unique_ptr<KoblazeSavedLayout> saved = std::move(_koblazeSavedLayout);
    // A layout with the same styles adds no nodes. Do not put geometry on
    // another tree.
    if ( !saved || saved->elements != _elemCount || saved->texts != _textCount )
        return false;
    clearRendBlockCache();
    resetNodeNumberingProps();
    for ( int i = 0; i <= _elemCount; ++i ) {
        ldomNode * node = &_elemList[i >> TNC_PART_SHIFT][i & TNC_PART_MASK];
        if ( !node->isElement() )
            continue;
        node->setRenderData(saved->rects[i]);
        node->setRendMethod((lvdom_element_render_method)saved->methods[i]);
    }
    _koblazeChangedMethods = saved->changedMethods;
    _koblazeChangedMethodsKnown = saved->changedMethodsKnown;
    _koblazeRelayout = false;
    _koblazeSliceFirst = saved->sliceFirst;
    _koblazeSliceLast = saved->sliceLast;
    _koblazeSpineItemChars = saved->spineItemChars;
    _pagesData.swap(saved->pages);
    _hdr = saved->header;
    _nodeStyleHash = 0;
    _doc_rendering_hash = _hdr.getRenderingHash();
    _pagesData.setPos(0);
    pages->clear();
    if ( !pages->deserialize(_pagesData) ) {
        _rendered = false;
        return false;
    }
    _rendered = true;
    _toc_from_cache_valid = false;
    m_toc.invalidatePageNumbers();
    m_pagemap.invalidatePageInfo();
    return true;
}

int ldomDocument::koblazeTextLength( ldomNode * node )
{
    const lUInt32 key = node->getDataIndex();
    std::unordered_map<lUInt32, int>::const_iterator known = _koblazeTextLengths.find(key);
    if ( known != _koblazeTextLengths.end() )
        return known->second;
    int length = 0;
    if ( node->isText() ) {
        length = node->getText().length();
    }
    else {
        const int count = node->getChildCount();
        for ( int i = 0; i < count; i++ )
            length += koblazeTextLength( node->getChildNode(i) );
    }
    _koblazeTextLengths[key] = length;
    return length;
}

int ldomDocument::koblazeTextBefore( ldomNode * root, ldomNode * target )
{
    int before = 0;
    for ( ldomNode * node = target; node != root; ) {
        ldomNode * parent = node->getParentNode();
        if ( !parent )
            return -1;
        const int count = parent->getChildCount();
        std::vector<int> & prefix = _koblazeTextPrefixes[parent->getDataIndex()];
        if ( (int)prefix.size() != count ) {
            // The characters before each child. A layout can add boxes, so
            // a list of another size is counted again.
            prefix.assign(count, 0);
            int sum = 0;
            for ( int i = 0; i < count; i++ ) {
                prefix[i] = sum;
                sum += koblazeTextLength( parent->getChildNode(i) );
            }
        }
        const int index = node->getNodeIndex();
        if ( index < 0 || index >= count )
            return -1;
        before += prefix[index];
        node = parent;
    }
    return before;
}

// A block without text, such as an image, has no characters to place it by:
// it sits at the count of the text before it. At the border of two slices
// that count is in both. Such a block belongs to the slice that ends at it,
// so that it is laid out once: a slice starts with a block that has text.
bool ldomDocument::koblazeEmptyBlockInSlice( int start ) const
{
    if ( _koblazeSliceFirst <= 0 )
        return start <= _koblazeSliceLast;
    return start > _koblazeSliceFirst && start <= _koblazeSliceLast;
}

// A table adds its lines to the pages without the flow, so the page break
// rules after it come from the block before the table. When the block before
// the slice is a table, the block before that table is laid out too.
bool ldomDocument::koblazeTableEndsSliceAfter( ldomNode * node, int end )
{
    ldomNode * next = NULL;
    for ( ldomNode * n = node; n && !next && n->getNodeId() != el_DocFragment; n = n->getParentNode() ) {
        ldomNode * parent = n->getParentNode();
        if ( !parent )
            return false;
        const int count = parent->getChildCount();
        for ( int i = n->getNodeIndex() + 1; i < count && !next; i++ ) {
            ldomNode * child = parent->getChildNode(i);
            if ( koblazeTextLength(child) > 0 && !child->isElement() )
                return false;
            if ( child->isElement() && child->getRendMethod() != erm_invisible )
                next = child;
        }
    }
    // The first block in it, through containers.
    while ( next && next->getRendMethod() == erm_block ) {
        ldomNode * first = NULL;
        const int count = next->getChildCount();
        for ( int i = 0; i < count && !first; i++ ) {
            ldomNode * child = next->getChildNode(i);
            if ( koblazeTextLength(child) > 0 && !child->isElement() )
                return false;
            if ( child->isElement() && child->getRendMethod() != erm_invisible )
                first = child;
        }
        next = first;
    }
    return next && next->getRendMethod() == erm_table
            && end + koblazeTextLength(next) == _koblazeSliceFirst;
}

bool ldomDocument::koblazeEnterBlock( ldomNode * node, bool spineItem, bool container )
{
    if ( _koblazeSliceLast <= 0 || spineItem )
        return true;
    // Use the same offsets as saved passages. Hidden blocks and floats
    // are not always visited by the main flow.
    ldomNode * body = node;
    for ( ldomNode * parent = node->getParentNode(); parent; parent = parent->getParentNode() ) {
        if ( parent->getNodeId() == el_DocFragment )
            break;
        if ( parent->getRendMethod() != erm_block )
            return true;
        body = parent;
    }
    if ( body->getNodeId() != el_body || !body->getParentNode() ||
            body->getParentNode()->getNodeId() != el_DocFragment )
        return true;
    const int length = koblazeTextLength(node);
    if ( node == body && length > _koblazeSpineItemChars )
        _koblazeSpineItemChars = length;
    const int start = koblazeTextBefore(body, node);
    if ( node->getNodeId() == el_floatBox && node->isFloatingBox() )
        _koblazeSliceHasFloats = true;
    if ( container ) {
        // A container with text in the slice is entered, and its blocks are
        // placed by their source offsets. Containers outside the slice are left out as
        // a whole, with its margins and page breaks.
        if ( length > 0 && start + length > _koblazeSliceFirst && start < _koblazeSliceLast )
            return true;
        if ( length == 0 && koblazeEmptyBlockInSlice(start) )
            return true;
        // The container that ends where the slice starts holds the block
        // before the slice. See below.
        if ( length > 0 && _koblazeSliceFirst > 0 && start + length == _koblazeSliceFirst )
            return true;
        if ( length > 0 && _koblazeSliceFirst > 0 && start + length < _koblazeSliceFirst
                && koblazeTableEndsSliceAfter(node, start + length) )
            return true;
    }
    if ( container )
        return false;
    const bool inside = length > 0 ? start >= _koblazeSliceFirst && start < _koblazeSliceLast
                                   : koblazeEmptyBlockInSlice(start);
    if ( inside && !_koblazeFirstBlock && _koblazeSliceFirst > 0 )
        _koblazeFirstBlock = node;
    // The space above the first block of the slice depends on the block
    // before it: their margins collapse. That block is laid out too, so
    // that the first page is the page of the whole spine item. Its lines
    // are not added to the pages: see koblazeSkipLinesUntil().
    if ( !inside && !_koblazeFirstBlock && _koblazeSliceFirst > 0
            && ( length > 0 ? start + length == _koblazeSliceFirst : start == _koblazeSliceFirst ) )
        return true;
    if ( !inside && !_koblazeFirstBlock && _koblazeSliceFirst > 0 && length > 0
            && start + length < _koblazeSliceFirst && koblazeTableEndsSliceAfter(node, start + length) )
        return true;
    return inside;
}

// Layout snapshots live only as long as their document. They retain immutable
// styles and node geometry; text, resources and the DOM are shared. Fonts are
// resolved through the normal cache-loading path when restoring a layout.
bool ldomDocument::retainLayout(size_t maxBytes)
{
    clearAlternateLayout();
    if (!_rendered || _partial_rerendering_enabled || _pagesData.error())
        return false;
    const size_t base = sizeof(LayoutSnapshot) + _pagesData.size();
    if (base > maxBytes || size_t(_elemCount + 1) > (maxBytes - base) / sizeof(LayoutNode))
        return false;
    std::unique_ptr<LayoutSnapshot> snapshot(new LayoutSnapshot(_pagesData.size()));
    snapshot->nodes.reserve(_elemCount + 1);
    snapshot->bytes = base + size_t(_elemCount + 1) * sizeof(LayoutNode);
    std::set<css_style_rec_t *> styles;
    for (int i = 0; i <= _elemCount; ++i) {
        ldomNode *node = &_elemList[i >> TNC_PART_SHIFT][i & TNC_PART_MASK];
        if (!node->isElement())
            continue;
        LayoutNode entry;
        entry.node = node;
        entry.parent = node->getParentNode() ? node->getParentNode()->getDataIndex() : 0;
        entry.name = node->getNodeId();
        entry.children = node->getChildCount();
        node->getRenderData(entry.rect);
        // Snapshots own a style reference, not another node index reference.
        entry.style = _styles.get(getNodeStyleIndex(node->getDataIndex()));
        entry.method = node->getRendMethod();
        // Account for unique retained styles, including their variable data.
        if (!entry.style.isNull() && styles.insert(entry.style.get()).second) {
            SerialBuf style(1024, false);
            entry.style->serialize(style);
            snapshot->bytes += sizeof(css_style_rec_t) + style.size();
            if (style.error() || snapshot->bytes > maxBytes)
                return false;
        }
        snapshot->nodes.push_back(entry);
    }
    snapshot->pages << _pagesData;
    if (snapshot->pages.error())
        return false;
    snapshot->header = _hdr;
    snapshot->maxBytes = maxBytes;
    snapshot->elements = _elemCount;
    snapshot->texts = _textCount;
    _alternateLayout = std::move(snapshot);
    return true;
}

bool ldomDocument::swapLayout(LVRendPageList *pages)
{
    if (!_alternateLayout || !_rendered || _partial_rerendering_enabled)
        return false;
    LayoutSnapshot &snapshot = *_alternateLayout;
    if (snapshot.elements != _elemCount || snapshot.texts != _textCount) {
        clearAlternateLayout();
        return false;
    }
    // A render can add auto-boxes. Do not apply geometry to a different tree.
    for (const LayoutNode &entry : snapshot.nodes) {
        ldomNode *node = entry.node;
        if (!node->isElement() || node->getNodeId() != entry.name ||
            node->getChildCount() != entry.children ||
            (node->getParentNode() ? node->getParentNode()->getDataIndex() : 0) != entry.parent) {
            clearAlternateLayout();
            return false;
        }
    }
    // Capture the departing layout before changing any geometry. Both snapshots
    // fit the same cap; two exist only during this exchange.
    std::unique_ptr<LayoutSnapshot> target = std::move(_alternateLayout);
    retainLayout(target->maxBytes);
    clearRendBlockCache();
    resetNodeNumberingProps();
    for (LayoutNode &entry : target->nodes) {
        entry.node->setRenderData(entry.rect);
        // The cache loader rebuilds font references from empty node indices.
        clearNodeStyle(entry.node->getDataIndex());
        entry.node->setStyle(entry.style);
        entry.node->setRendMethod(entry.method);
    }
    _koblazeChangedMethods.clear();
    _koblazeChangedMethodsKnown = false;
    if (!updateLoadedStyles(true)) {
        clearAlternateLayout();
        _rendered = false;
        return false;
    }
    _pagesData.swap(target->pages);
    _hdr = target->header;
    _nodeStyleHash = 0;
    _doc_rendering_hash = _hdr.getRenderingHash();
    _pagesData.setPos(0);
    pages->clear();
    if (!pages->deserialize(_pagesData)) {
        clearAlternateLayout();
        _rendered = false;
        return false;
    }
    _toc_from_cache_valid = false;
    m_toc.invalidatePageNumbers();
    m_pagemap.invalidatePageInfo();
    setCacheFileStale(true);
    return true;
}
#endif

bool LVDocView::swapLayout() {
    if (!m_doc || !m_doc->swapLayout(&m_pages))
        return false;
    m_section_bounds_valid = false;
    _posIsSet = false;
    clearImageCache();
    return true;
}

bool LVDocView::koblazeSaveLayout() {
    return m_doc && m_is_rendered && m_doc->koblazeSaveLayout();
}

bool LVDocView::koblazeRestoreLayout() {
    if (!m_doc || !m_doc->koblazeRestoreLayout(&m_pages))
        return false;
    m_section_bounds_valid = false;
    _posIsSet = false;
    clearImageCache();
    m_is_rendered = true;
    return true;
}
