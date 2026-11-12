/** \file lvpagesplitter.h
    \brief page splitter interface

    CoolReader Engine

    (c) Vadim Lopatin, 2000-2006
    This source code is distributed under the terms of
    GNU General Public License.

    See LICENSE file for details.
*/

#ifndef __LV_PAGESPLITTER_H_INCLUDED__
#define __LV_PAGESPLITTER_H_INCLUDED__

#include "crsetup.h"

#include <stdlib.h>
#include <time.h>
#include "lvtypes.h"
#include "lvarray.h"
#include "lvptrvec.h"
#include "lvref.h"
#include "lvstring.h"
#include "lvhashtable.h"

#ifndef RENDER_PROGRESS_INTERVAL_MILLIS
#define RENDER_PROGRESS_INTERVAL_MILLIS 300
#endif
#ifndef RENDER_PROGRESS_INTERVAL_PERCENT
#define RENDER_PROGRESS_INTERVAL_PERCENT 2
#endif

/// &7 values
#define RN_SPLIT_AUTO   0
#define RN_SPLIT_AVOID  1
#define RN_SPLIT_ALWAYS 2
/// right-shift
#define RN_SPLIT_BEFORE 0
#define RN_SPLIT_AFTER  3

#define RN_SPLIT_BEFORE_AUTO   (RN_SPLIT_AUTO<<RN_SPLIT_BEFORE)
#define RN_SPLIT_BEFORE_AVOID  (RN_SPLIT_AVOID<<RN_SPLIT_BEFORE)
#define RN_SPLIT_BEFORE_ALWAYS (RN_SPLIT_ALWAYS<<RN_SPLIT_BEFORE)
#define RN_SPLIT_AFTER_AUTO    (RN_SPLIT_AUTO<<RN_SPLIT_AFTER)
#define RN_SPLIT_AFTER_AVOID   (RN_SPLIT_AVOID<<RN_SPLIT_AFTER)
#define RN_SPLIT_AFTER_ALWAYS  (RN_SPLIT_ALWAYS<<RN_SPLIT_AFTER)

#define RN_SPLIT_BOTH_AUTO      RN_SPLIT_BEFORE_AUTO | RN_SPLIT_AFTER_AUTO
#define RN_SPLIT_BOTH_AVOID    RN_SPLIT_BEFORE_AVOID | RN_SPLIT_AFTER_AVOID

#define RN_SPLIT_FOOT_NOTE 0x100
#define RN_SPLIT_FOOT_LINK 0x200

#define RN_SPLIT_DISCARD_AT_START 0x400

#define RN_LINE_IS_RTL 0x1000
#define RN_LINE_JUSTIFIABLE_TEXT 0x2000
#define RN_LINE_FIXED_SPACING 0x800
#define RN_LINE_SPACING_GROUP_START 0x10000
#define RN_LINE_SPACING_GROUP_END 0x20000
#define RN_LINE_ANCHORED_SPACING 0x80
#define RN_LINE_WHITESPACE 0x4000
#define RN_LINE_PARAGRAPH_PROTECTION 0x8000
#define RN_PAGE_PARAGRAPH_PROTECTION_GAP 0x80
#define RN_PAGE_KEEP_VERTICAL_SPACING 0x40

#define RN_GET_SPLIT_BEFORE(flags) ((flags >> RN_SPLIT_BEFORE) & 0x7)
#define RN_GET_SPLIT_AFTER(flags)  ((flags >> RN_SPLIT_AFTER)  & 0x7)

#define RN_PAGE_TYPE_NORMAL           0x01
#define RN_PAGE_TYPE_COVER            0x02
#define RN_PAGE_MOSTLY_RTL            0x10
#define RN_PAGE_FOOTNOTES_MOSTLY_RTL  0x20

/// footnote fragment inside page
class LVPageFootNoteInfo {
public:
    int start;
    int height;
    LVPageFootNoteInfo()
    : start(0), height(0)
    { }
    LVPageFootNoteInfo( int s, int h )
    : start(s), height(h) 
    { }
};

template <typename T, int RESIZE_MULT, int RESIZE_ADD> class CompactArray
{
    struct Array {
        T * _list;
        int _size;
        int _length;
        Array()
        : _list(NULL), _size(0), _length(0)
        {
        }
        ~Array()
        {
            clear();
        }
        void add( T item )
        {
            if ( _size<=_length ) {
                _size = _size*RESIZE_MULT + RESIZE_ADD;
                _list = cr_realloc( _list, _size );
            }
            _list[_length++] = item;
        }
        void add( T * items, int count )
        {
            if ( count<=0 )
                return;
            if ( _size<_length+count ) {
                _size = _length+count;
                _list = cr_realloc( _list, _size );
            }
            for ( int i=0; i<count; i++ )
                _list[_length+i] = items[i];
            _length += count;
        }
        void reserve( int count )
        {
            if ( count<=0 )
                return;
            if ( _size<_length+count ) {
                _size = _length+count;
                _list = cr_realloc( _list, _size );
            }
        }
        void clear()
        {
            if ( _list ) {
                free( _list );
                _list = NULL;
                _size = 0;
                _length = 0;
            }
        }
        int length() const
        {
            return _length;
        }
        T get( int index ) const
        {
            return _list[index];
        }
        const T & operator [] (int index) const
        {
            return _list[index];
        }
        T & operator [] (int index)
        {
            return _list[index];
        }
    };

    Array * _data;
public:
    CompactArray()
    : _data(NULL)
    {
    }
    ~CompactArray()
    {
        if ( _data )
            delete _data; // NOLINT(clang-analyzer-cplusplus.NewDelete)
    }
    void add( T item )
    {
        if ( !_data )
            _data = new Array();
        _data->add(item);
    }
    void add( T * items, int count )
    {
        if ( !_data )
            _data = new Array();
        _data->add(items, count);
    }
    void add( LVArray<T> & items )
    {
        if ( items.length()<=0 )
            return;
        if ( !_data )
            _data = new Array();
        _data->add( &(items[0]), items.length() );
    }
    void reserve( int count )
    {
        if ( count<=0 )
            return;
        if ( !_data )
            _data = new Array();
        _data->reserve( count );
    }
    void clear()
    {
        if ( _data ) {
            delete _data;
            _data = NULL;
        }
    }
    int length() const
    {
        return _data ? _data->length() : 0;
    }
    T get( int index ) const
    {
        return _data->get(index);
    }
    const T & operator [] (int index) const
    {
        return _data->operator [](index);
    }
    T & operator [] (int index)
    {
        return _data->operator [](index);
    }
    bool empty() { return !_data || _data->length()==0; }

};

/// rendered page splitting info
class LVRendPageInfo {
public:
    // Source boxes and the boundaries at which extra leading is allowed.
    // Fixed groups share one offset, including their adjacent margins.
    LVArray<lvPoint> justificationLines;
    LVArray<int> justificationGaps;
    int justificationProseLines = 0;
    int justificationProseHeight = 0;
    int justificationPrefix = 0;
    void clearJustification() {
        justificationLines.clear();
        justificationGaps.clear();
        justificationProseLines = justificationProseHeight = justificationPrefix = 0;
    }
    int justificationExtra(int available) const {
        int count = justificationLines.length(), gaps = justificationGaps.length();
        if (!count || !gaps || justificationProseLines < 6 || footnotes.length() ||
            (flags & RN_PAGE_KEEP_VERTICAL_SPACING)) return 0;
        int extra = available - justificationLines[count-1].y;
        int average = justificationProseHeight / justificationProseLines;
        return extra > 0 && extra <= available / 8 && extra <= gaps * average / 8 ? extra : 0;
    }
    int centeringOffset(int available) const {
        if (!justificationLines.length() || !justificationProseLines || footnotes.length() || available <= justificationPrefix) return 0;
        int first = 0;
        while (first < justificationLines.length() && justificationLines[first].x < justificationPrefix) ++first;
        if (first == justificationLines.length()) return 0;
        int top = justificationLines[first].x;
        int occupied = justificationLines[justificationLines.length()-1].y - top;
        int room = available - justificationPrefix;
        if (occupied > room || occupied * 10 < room * 9) return 0;
        int offset = (room - occupied) / 2 - (top - justificationPrefix);
        // An opening float stays attached to its paragraph. Do not pull the
        // following text into the protected prefix or its authored margin.
        return justificationPrefix && offset < 0 ? 0 : offset;
    }
    int justificationOffset(int y, int available, int mode, bool bottom=false) const {
        if (bottom) --y;
        if (y < justificationPrefix) return 0;
        if (mode == 2) return centeringOffset(available);
        int extra = justificationExtra(available);
        if (!extra) return 0;
        int lo=0, hi=justificationGaps.length();
        while (lo < hi) {
            int mid=(lo+hi)/2;
            if (justificationGaps[mid] <= y) lo=mid+1;
            else hi=mid;
        }
        return extra * lo / justificationGaps.length();
    }
    int justificationSourceY(int y, int available, int mode, bool pull) const {
        if (mode == 2) {
            if (y < justificationPrefix) return y;
            int offset = centeringOffset(available);
            if (justificationPrefix && y < justificationPrefix + offset)
                return pull ? justificationPrefix : -1;
            return y - offset;
        }
        int extra = justificationExtra(available);
        if (!extra) return y;
        int previous = 0;
        for (int i=0; i<justificationGaps.length(); ++i) {
            int offset = extra * (i+1) / justificationGaps.length();
            int edge = justificationGaps[i];
            if (y < edge + previous) return y - previous;
            if (y < edge + offset) return pull ? edge : -1;
            previous = offset;
        }
        return y - previous;
    }
    int start; /// start of page

    int index;  /// index of page
    lInt16 height; /// height of page, does not include footnotes
    lUInt8 flags;  /// RN_PAGE_*
    CompactArray<LVPageFootNoteInfo, 1, 4> footnotes; /// footnote fragment list for page
    lUInt16 flow;
    lUInt8 writing_mode; /// FORK: css_writing_mode_t of this page's content (per-page mixed writing mode)
    LVRendPageInfo(int pageStart, lUInt16 pageHeight, int pageIndex)
    : start(pageStart), index(pageIndex), height(pageHeight), flags(RN_PAGE_TYPE_NORMAL), flow(0), writing_mode(0) {}
    LVRendPageInfo(lUInt16 coverHeight)
    : start(0), index(0), height(coverHeight), flags(RN_PAGE_TYPE_COVER), flow(0), writing_mode(0) {}
    LVRendPageInfo()
    : start(0), index(0), height(0), flags(RN_PAGE_TYPE_NORMAL), flow(0), writing_mode(0) {}
    bool serialize( SerialBuf & buf );
    bool deserialize( SerialBuf & buf );
};

class LVRendPageList : public LVPtrVector<LVRendPageInfo>
{
    bool has_nonlinear_flows;
    bool has_mixed_writing_modes; // FORK: pages have differing writing modes
public:
    LVRendPageList() : has_nonlinear_flows(false), has_mixed_writing_modes(false) {}
    int FindNearestPage( int y, int direction );
    void setHasNonLinearFlows( bool hasnonlinearflows ) { has_nonlinear_flows = hasnonlinearflows; }
    bool hasNonLinearFlows() { return has_nonlinear_flows; }
    void setHasMixedWritingModes( bool v ) { has_mixed_writing_modes = v; }
    bool hasMixedWritingModes() { return has_mixed_writing_modes; }
    bool serialize( SerialBuf & buf );
    bool deserialize( SerialBuf & buf );
    void replacePages( int old_y, int old_h, LVRendPageList * pages, int next_pages_shift_y );
};

class LVFootNote;

class LVFootNoteList;

class LVFootNoteList : public LVArray<LVFootNote*> {
public: 
    LVFootNoteList() {}
};


class LVRendLineInfo {
    friend struct PageSplitState;
    LVFootNoteList * links; // 4 bytes
    int start;              // 4 bytes
    int height;             // 4 bytes (we may get extra tall lines with tables TR)
public:
    lUInt32 flags;          // split rules and page-spacing classification
    lUInt16 flow;           // 2 bytes (should be enough)
    lUInt8 writing_mode;    // FORK: css_writing_mode_t of the content that produced this line
    int getSplitBefore() const { return (flags>>RN_SPLIT_BEFORE)&7; }
    int getSplitAfter() const { return (flags>>RN_SPLIT_AFTER)&7; }
/*
    LVRendLineInfo & operator = ( const LVRendLineInfoBase & v )
    {
        start = v.start;
        end = v.end;
        flags = v.flags;
        return *this;
    }
*/
    bool empty() const { 
        return start==-1; 
    }

    void clear() { 
        start = -1; height = 0; flags = 0;
        if ( links!=NULL ) {
            delete links; 
            links=NULL;
        } 
    }

    inline int getEnd() const { return start + height; }
    inline int getStart() const { return start; }
    inline int getHeight() const { return height; }
    inline lUInt32 getFlags() const { return flags; }

    LVRendLineInfo() : links(NULL), start(-1), height(0), flags(0), flow(0), writing_mode(0) { }
    LVRendLineInfo( int line_start, int line_end, lUInt32 line_flags )
    : links(NULL), start(line_start), height(line_end-line_start), flags(line_flags), flow(0), writing_mode(0)
    {
    }
    LVRendLineInfo( int line_start, int line_end, lUInt32 line_flags, int flow )
    : links(NULL), start(line_start), height(line_end-line_start), flags(line_flags), flow(flow), writing_mode(0)
    {
    }
    LVFootNoteList * getLinks() { return links; }
    ~LVRendLineInfo()
    {
        clear();
    }
    int getLinksCount()
    {
        if ( links==NULL )
            return 0;
        return links->length();
    }
    void addLink( LVFootNote * note, int pos=-1 )
    {
        if ( links==NULL )
            links = new LVFootNoteList();
        if ( pos >= 0 ) // insert at pos
            links->insert( pos, note );
        else // append
            links->add( note );
        flags |= RN_SPLIT_FOOT_LINK;
    }
};


typedef LVFastRef<LVFootNote> LVFootNoteRef;

// A LVFootNote is created when we meet a <a href=> with some internal link, and may
// later be fed with "lines" of footnote text, and then it becomes actual.
// When not yet actual, it may be diverted to another already created LVFootnote (when
// an actual footnote contains, and is then associated to, multiple id=).
class LVFootNote : public LVRefCounter {
    lString32 id;
    bool is_actual; // set when a LVFootNote holds text (it should then never become a proxy to another one)
    LVFootNote * actual_footnote; // when set, this LVFootNote is a proxy to this actual_footnote
    CompactArray<LVRendLineInfo*, 2, 4> lines;
public:
    LVFootNote( lString32 noteId )
        : id(noteId), is_actual(false), actual_footnote(NULL)
    {
    }
    bool isActual() {
        return is_actual;
    }
    void setIsActual( bool actual ) {
        is_actual = actual;
        if ( actual ) // clean any previously set proxy
            actual_footnote = NULL;
    }
    LVFootNote * getActualFootnote() {
        return actual_footnote;
    }
    void setActualFootnote( LVFootNote * actualfootnote ) {
        actual_footnote = actualfootnote;
    }
    void addLine( LVRendLineInfo * line )
    {
        lines.add( line );
    }
    // CompactArray<LVRendLineInfo*, 2, 4> & getLines() { printf("getLines %x\n", lines); return lines; }
    LVRendLineInfo * getLine(int index) {
        if ( actual_footnote )
            return actual_footnote->lines[index];
        return lines[index];
    }
    int  length() {
        if ( actual_footnote )
            return actual_footnote->lines.length();
        return lines.length();
    }
    bool empty() {
        if ( actual_footnote )
            return actual_footnote->lines.empty();
        return lines.empty();
    }
    void clear() { lines.clear(); }
    lString32 getId() { return id; }
};

class LVDocViewCallback;
struct ldomNode;

class LVRendPageContext
{


    LVPtrVector<LVRendLineInfo> lines;

    LVDocViewCallback * callback;
    ldomNode ** koblaze_started = NULL;
    int totalFinalBlocks;
    int renderedFinalBlocks;
    int lastPercent;
    CRTimerUtil progressTimeout;


    // page start line
    //LVRendLineInfoBase pagestart;
    // page end candidate line
    //LVRendLineInfoBase pageend;
    // next line after page end candidate
    //LVRendLineInfoBase next;
    // last fit line
    //LVRendLineInfoBase last;
    // page list to fill
    LVRendPageList * page_list;
    // page height
    int page_h;
    // For vertical-rl/lr: page-split boundary stride (= page_width).
    // When non-zero, split() uses this instead of page_h, so vertical
    // documents can keep the column-length page_h (for the text formatter)
    // while still having the page splitter break at every page_width of
    // horizontal advance.  Zero means "use page_h" (default / horizontal).
    int vert_split_page_h;
    // document default font size (= root node font size)
    int doc_font_size;
    // Whether to gather lines or not (only footnote links will be gathered if not)
    bool gather_lines;
    // Links gathered when !gather_lines
    lString32Collection link_ids;
    // current flow being processed
    int current_flow;
    // maximum flow encountered so far
    int max_flow;
    // to know if current flow got some lines
    bool current_flow_empty;
    // FORK: per-page mixed writing-mode support.
    // current_writing_mode is stamped onto every AddLine()d line; the layout
    // (FlowState) updates it when it switches mode for a DocFragment.  When two
    // different modes are ever seen, has_mixed_writing_modes is set, which makes
    // the page splitter pick the stride (page_width vs page_h) per page.  Pure
    // single-mode documents never set the flag, so they take the unchanged path.
    int current_writing_mode;
    int first_writing_mode;       // baseline mode (-1 until first set)
    bool has_mixed_writing_modes;

    LVHashTable<lString32, LVFootNoteRef> footNotes;

    LVFootNote * curr_note;
    lString32 curr_note_id;

    // Note: a footnote indexed in 'footNotes' may have pointers to it stored in other
    // footnotes' actual_footnote. 'footNotes' is usually the only thing holding a
    // reference to a LVFootNote. If we replace with footNotes.set() an existing
    // footnote, it may be deleted, and using these other footnotes' actual_footnote
    // could cause segfault. So, below, we should avoid doing that, and prefer just
    // erasing a previous footnote's 'lines'.
    // In the following, we want to handle as well as possible the edge case
    // of buggy books having duplicated id= among footnotes (which may happen
    // in Wikipedia EPUBs), which makes things a tad more complex...
    LVFootNoteRef getOrCreateFootNote( lString32 id, bool actual, bool allow_appending )
    {
        LVFootNoteRef ref = footNotes.get(id);
        if ( ref.isNull() ) {
            // Not found: create one and index it
            ref = LVFootNoteRef( new LVFootNote( id ) );
            footNotes.set( id, ref );
            if ( actual ) {
                ref.get()->setIsActual(true);
            }
        }
        else {
            // Found an existing one
            if ( actual ) {
                // We are going to add lines
                if ( ref.get()->isActual() && ! allow_appending ) {
                    // If the one we found is already actual, something is wrong: this may
                    // happen with buggy books with duplicated id= (ie. Wikipedia EPUBs...).
                    // LVPageSplitter expects a footnote to be a single chunk/slice of the
                    // document, so we can't accumulate lines from different places: override
                    // its content. (This is consistent with the way crengine handle id= when
                    // building the DOM: later ones override ealier ones).
                    // Exception: When `allow_appending` we extend an already created footnote
                    // and trust that the caller has ensured that this is contiguous with
                    // already accumulated lines by calling `getCurrentFootNoteId`
                    ref.get()->clear();
                }
                // Make a non-actual (which may be a proxy or not) actual
                ref.get()->setIsActual(true);
            }
            // if actual=false, we're all fine, any kind will do.
        }
        return ref;
    }
    LVFootNoteRef getOrCreateFootNote( lString32Collection & ids, bool allow_appending )
    {
        // This, with multiple ids, is always called to create an actual footnote,
        // so no need to handle any actual=false case.
        if (ids.length() == 1) { // use single id method.
            return getOrCreateFootNote(ids.at(0), true, allow_appending);
        }
        // Multiple ids provided: zero, one, or more of them may exist
        LVFootNoteRef ref;
        int found = -1;
        for ( int n=0; n<ids.length(); n++ ) {
            ref = footNotes.get(ids.at(n));
            if ( !ref.isNull() ) {
                found = n;
                // As above, see comments there.
                if ( ref.get()->isActual() && ! allow_appending ) {
                    ref.get()->clear();
                }
                ref.get()->setIsActual(true);
                break;
            }
        }
        if ( found < 0 ) {
            // None found: create one with the first of the ids provided
            ref = LVFootNoteRef( new LVFootNote( ids.at(0) ) );
            ref.get()->setIsActual(true);
        }
        // Need to do something for each of the ids provided
        for ( int n=0; n<ids.length(); n++ ) {
            if (found < 0 ) {
                // No existing footnote found: index with this id the same LVFootnote we created
                footNotes.set( ids.at(n), ref );
            }
            else if ( n == found ) {
                // The one we found (so, already indexed), and that we will return and
                // that will get lines: nothing else to do.
            }
            else {
                // Other ids may alredy exist or not
                LVFootNoteRef nref = footNotes.get(ids.at(n));
                if ( nref.isNull() ) {
                    // No existing footnote with this id: index with this id the same LVFootnote we created
                    footNotes.set( ids.at(n), ref );
                }
                else {
                    // Existing other footnote associated to one of our multiple ids:
                    if ( nref.get()->isActual() ) {
                        // That other footnote is already actual, holding lines: it feels it's best
                        // to not do anything and let it be, and don't associate to this footnote.
                    }
                    else {
                        // Not actual: make it a proxy to the footnote we will return
                        // (and that will get lines)
                        nref.get()->setActualFootnote( ref.get() );
                    }
                }
            }
        }
        return ref;
    }

    void split();
public:


    /// koblaze: while `*started` is null, lines are not added. A slice that
    /// does not start at the start of its spine item must begin its first
    /// page at the top of its first block, as the page of the whole spine
    /// item does. What the containers of that block put above it is not a
    /// part of that page.
    void koblazeSkipLinesUntil( ldomNode ** started ) { koblaze_started = started; }
    void setCallback(LVDocViewCallback * cb, int _totalFinalBlocks) {
        callback = cb; totalFinalBlocks=_totalFinalBlocks;
        progressTimeout.restart(RENDER_PROGRESS_INTERVAL_MILLIS);
    }
    bool updateRenderProgress( int numFinalBlocksRendered );

    bool wantsLines() { return gather_lines; }

    void newFlow( bool nonlinear );

    /// Get the number of links in the current line links list, or
    // in link_ids when !gather_lines
    int getCurrentLinksCount();

    /// append or insert footnote link to last added line
    void addLink( lString32 id, int pos=-1 );

    /// get gathered links when !gather_lines
    // (returns a reference to avoid lString32Collection destructor from
    // being called twice and a double free crash)
    lString32Collection * getLinkIds() { return &link_ids; }

    /// mark start of foot note
    void enterFootNote( lString32 id );
    void enterFootNote( lString32Collection & ids, bool appending );

    /// mark end of foot note
    void leaveFootNote();

    lString32 getCurrentFootNoteId() {
        return curr_note_id;
    }

    /// returns page height
    int getPageHeight() { return page_h; }

    /// set page height (used for vertical text: temporarily swapped with page width)
    void setPageHeight( int h ) { page_h = h; }

    /// for vertical-rl/lr: tell the page splitter to break at every `h` units
    /// of horizontal advance instead of page_h.  Pass 0 to disable (default).
    void setVerticalSplitPageHeight( int h ) { vert_split_page_h = h; }
    int  getVerticalSplitPageHeight() { return vert_split_page_h; }
    /// effective page-split stride: vertical override if set, else page_h
    int  getEffectivePageHeight() { return vert_split_page_h > 0 ? vert_split_page_h : page_h; }

    /// FORK (per-page mixed writing mode): the layout sets the writing mode of
    /// the content currently being added; AddLine() stamps it onto each line.
    /// Switching to a second distinct mode flags the document as mixed.
    void setCurrentWritingMode( int wm ) {
        current_writing_mode = wm;
        if ( first_writing_mode < 0 )
            first_writing_mode = wm; // baseline = document primary mode
        else if ( wm != first_writing_mode )
            has_mixed_writing_modes = true;
    }
    bool hasMixedWritingModes() { return has_mixed_writing_modes; }

    /// returns document font size
    int getDocFontSize() { return doc_font_size; }

    /// returns page list pointer
    LVRendPageList * getPageList() { return page_list; }

    /// constructor (docFontSize is only needed for with main context actually used to split pages)
    LVRendPageContext(LVRendPageList * pageList, int pageHeight, int docFontSize=0, bool gatherLines=true);

    /// add source line
    void AddLine( int starty, int endy, int flags );

    LVPtrVector<LVRendLineInfo> * getLines() {
        return &lines;
    };
    void Finalize();
    /// koblaze: forget this context after a cancelled layout
    void Abandon();
};

#endif

