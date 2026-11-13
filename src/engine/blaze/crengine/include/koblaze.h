/** \file koblaze.h
    \brief features of the koblaze fork that stock CREngine does not have

    A host that can also build with stock CREngine checks for this header.
*/
#ifndef __KOBLAZE_H_INCLUDED__
#define __KOBLAZE_H_INCLUDED__

#define KOBLAZE 1

/// Load the text of only the spine items from `first` to `last` of the EPUB
/// documents that are opened after this call. Every other spine item gets an
/// empty DocFragment, so the index of each DocFragment stays the same as in
/// a full load. A negative `first` loads every spine item, which is the
/// default.
/// Experimental: to show other spine items, the host opens the document
/// again with another window.
void koblazeSetSpineWindow(int first, int last);
/// True when the spine item is loaded with its text.
bool koblazeSpineItemLoaded(int index);
/// True when only a window of spine items is loaded with its text.
bool koblazeSpineWindowSet();

/// Take the pair kerning of plain left-to-right horizontal text from a
/// memory of glyph pairs, which HarfBuzz fills, and shape those runs
/// without the kern feature. A face checks the result against HarfBuzz
/// on a sample of its runs and goes back to HarfBuzz for good when they
/// differ. Off by default.
#define KOBLAZE_PAIR_KERNING 1
void koblazeSetPairKerning(bool enabled);
bool koblazePairKerning();

/// Measure plain left-to-right Latin text word by word. HarfBuzz shapes
/// each different word once; the face keeps its widths, and the kerning
/// between a word and the space next to it, in memory. Text is measured
/// from that memory with no shaping. A run that holds anything but such
/// words is shaped as before. A face checks the result against HarfBuzz on
/// a sample of its runs and goes back to HarfBuzz for good when they
/// differ. Only measuring changes; drawing shapes as before. Off by default.
#define KOBLAZE_WORD_WIDTHS 1
void koblazeSetWordWidths(bool enabled);
bool koblazeWordWidths();

/// With word widths from memory: build the widths of a new word from its
/// characters and pairs of characters, so that HarfBuzz is asked once for
/// each character and each pair, not once for each word. A word with a
/// character or a pair that does not give plain glyphs (a ligature, a
/// mark) is shaped as a whole. A face checks its first words and a sample
/// of the later ones against HarfBuzz and stops building words when one
/// differs. A font rule that needs three or more plain glyphs can pass
/// that check; such a word is then measured a kerning step off. Off by
/// default.
#define KOBLAZE_WORD_PAIRS 1
void koblazeSetWordPairs(bool enabled);
bool koblazeWordPairs();

/// Do not run the bidirectional algorithm on a paragraph only because it
/// holds U+FEFF (zero width no-break space). Stock CREngine counts that
/// character as right-to-left, with the Arabic forms around it, so every
/// paragraph that holds one pays for the algorithm, which then finds
/// nothing. The layout is the same. Off by default.
#define KOBLAZE_PLAIN_JOINER 1
void koblazeSetPlainJoiner(bool enabled);
bool koblazePlainJoiner();

/// Thrown out of ldomDocument::render() and LVDocView::Render() when the
/// callback asks to stop the layout. The document keeps its DOM. Its layout
/// is not valid until the next render, which starts again from the start.
class KoblazeRenderCancelled {};

/// A host that loads a document in the background can ask to stop that work.
/// While a check is set, the parser and the style pass call it every few
/// nodes, and throw KoblazeRenderCancelled when it returns true. The layout
/// asks LVDocViewCallback::OnRenderCancelRequested() as before. A document
/// whose load or style pass was stopped is not usable: destroy it. NULL
/// removes the check, which is the default.
#define KOBLAZE_CANCEL_CHECK 1
typedef bool (*KoblazeCancelCheck)(void * context);
void koblazeSetCancelCheck(KoblazeCancelCheck check, void * context);

// The engine counts the open KoblazeNoCancel and KoblazeParserCanStop scopes
// for the one document in work. Work that pauses on its own thread while
// another thread uses the engine swaps its counts out and zeros in, and
// swaps them back when it goes on.
void koblazeSwapCancelScopes(int * noCancel, int * parserCanStop);
/// Throws KoblazeRenderCancelled when a check is set and asks to stop.
void koblazeCheckCancel();
/// A place where work on a document can wait but must not be stopped, such
/// as between two blocks that it writes to its cache file. A host that
/// runs such work beside other work sets a function that waits there. It
/// must not throw. NULL removes it, which is the default.
#define KOBLAZE_PAUSE_POINT 1
typedef void (*KoblazePausePoint)(void * context);
void koblazeSetPausePoint(KoblazePausePoint pause, void * context);
void koblazePausePoint();
/// The parser reads many kinds of files: chapters, the package, hyphenation
/// dictionaries. A stop is safe only while it reads a chapter. The check
/// of the parser does nothing unless one of these lives.
struct KoblazeParserCanStop {
    KoblazeParserCanStop();
    ~KoblazeParserCanStop();
};
void koblazeCheckCancelInParser();
/// While one lives, koblazeCheckCancel() does nothing. For a destructor that
/// does work with checks in it: a destructor must not throw.
struct KoblazeNoCancel {
    KoblazeNoCancel();
    ~KoblazeNoCancel();
};

/// A document with DOC_FLAG_ENABLE_DOC_FONTS off (PROP_EMBEDDED_FONTS false
/// before the load) does not read the fonts that the book embeds. Stock
/// CREngine reads and unpacks them and then does not use them. A host whose
/// style sheet gives every element another font turns the flag off. The
/// document still lists the fonts in getEmbeddedFontList(). A style that
/// the host does not reach and that names such a font, for example one of
/// ::first-letter, reads the font when a font is first chosen for it, so
/// the text looks the same as with the flag on.
#define KOBLAZE_SKIP_DOCUMENT_FONTS 1

/// The font manager keeps the data of the fonts that a book embeds, so that
/// the next document of the same book does not read and unpack the font
/// files again. A host that loads a book chapter by chapter opens many
/// documents of one book. Call this when another book opens: the data is
/// found by file name and size, which only identify a font within one book.
#define KOBLAZE_DOCUMENT_FONT_DATA 1
void koblazeForgetDocumentFontData();

/// ldomDocument::setKoblazeDropEmptyPages() leaves out pages without height.
#define KOBLAZE_DROP_EMPTY_PAGES 1

/// ldomDocument::koblazeSaveLayout() and koblazeRestoreLayout() keep the
/// layout of a document aside while another layout is tried, for example
/// the next slice, and bring it back when that layout is stopped.
#define KOBLAZE_SAVED_LAYOUT 1

/// ldomDocument::koblazeTextLength() and koblazeTextBefore() count the text
/// of a node once and remember it.
#define KOBLAZE_TEXT_LENGTHS 1

/// ldomDocument::koblazeRelayout() lays out a document again with the styles
/// of its last layout, for example for another layout slice.
#define KOBLAZE_RELAYOUT 1

#endif
