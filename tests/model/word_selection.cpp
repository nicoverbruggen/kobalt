#include "reader_inputs.h"
#include "support/qt_compat.h"
#include "engine/crengine/engine_document.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QMap>
#include <QSet>
#include <cstdio>
#include "check.h"

int main(int argc, char **argv)
{
    QGuiApplication app(argc, argv);
    require(argc == 2, "Pass the synthetic dictionary EPUB");
    QFile file(QString::fromLocal8Bit(argv[1]));
    require(file.open(QIODevice::ReadOnly), "Cannot open fixture");
    kobalt::EngineDocument model(kobalt::test::readerFonts());
    QString error;
    require(model.open(file.readAll(), QSize(1072, 1300), {}, &error), qPrintable(error));
    QSet<QString> found;
    QMap<QString, kobalt::WordSelection> selected;
    for (int y = 0; y < 600; y += 10) {
        for (int x = 0; x < 1072; x += 12) {
            const auto word = model.wordAt({ x, y });
            if (word.text.isEmpty())
                continue;
            found.insert(word.text);
            selected.insert(word.text, word);
            require(!word.start.isEmpty() && !word.end.isEmpty() && !word.rectangles.isEmpty(),
                    "Selection lacks a source range or geometry");
        }
    }
    for (const auto *text :
         { "Hello", "world", "Reader’s", "don't", "café", "café", "naïve", "rainbow", "cooperation",
           "extraordinary", "A𐐀word", "日", "本", "語", "中", "文" }) {
        if (!found.contains(QString::fromUtf8(text))) {
            fprintf(stderr, "Missing %s; found: %s\n", text,
                    qPrintable(QStringList(found.values()).join(" | ")));
            return 1;
        }
    }
    const auto first = selected.value("Hello"), last = selected.value("world");
    QVector<int> sourceOrder;
    const auto samePage =
            model.pagesOf({ last.start, first.start, "invalid" }, false, &sourceOrder);
    require(samePage[0] == samePage[1] && sourceOrder[1] < sourceOrder[0] && sourceOrder[2] == -1,
            "Book order must use source positions within a page and leave unresolved positions unranked");
    bool foundSpace = false;
    for (const auto &gap : model.rangeRects(first.end, last.start)) {
        for (int x = gap.left(); x <= gap.right(); ++x) {
            const QPoint point(x, gap.center().y());
            if (model.annotationTokenAt(point).start == first.start
                && model.wordAt(point).text.isEmpty()) {
                foundSpace = true;
                require(model.annotationTokenAt(point).text == first.text,
                        "Whitespace did not select the preceding complete word");
            }
        }
    }
    require(foundSpace, "No selectable inter-word space found");
    require(model.annotationTokenAt({ 0, 0 }).text.isEmpty()
                    && model.annotationTokenAt({ -1, 100 }).text.isEmpty(),
            "Annotation whitespace fallback selected a margin or outside point");
    require(model.annotationDragTokenAt(last.rectangles.first().center(), first.end, false)
                    .text.isEmpty(),
            "End drag expanded before crossing the next word");
    bool crossedWord = false;
    for (const auto &word : selected) {
        const auto edge = model.annotationGeometry(word.start, word.end).end;
        if (!edge.visible)
            continue;
        for (int distance = 0; distance < 12; ++distance) {
            const QPoint beyond = (edge.first + edge.last) / 2 + edge.outward * distance;
            if (model.annotationTokenAt(beyond).start == word.start && word.start != first.start
                && model.annotationDragTokenAt(beyond, first.end, false).start == word.start)
                crossedWord = true;
        }
    }
    require(crossedWord, "End drag did not expand after crossing any complete word");
    require(model.annotationDragTokenAt(first.rectangles.first().center(), last.start, true)
                    .text.isEmpty(),
            "Start drag expanded before crossing the preceding word");
    require(model.annotationDragTokenAt(first.rectangles.first().center(), first.start, true).start
                    == first.start,
            "An unchanged boundary was rejected");
    const auto range = model.textRange(first.start, last.end);
    require(range.text.contains("Hello") && range.text.endsWith("world"),
            "Full boundary words missing");
    const auto reversed = model.textRange(last.end, first.start);
    require(reversed.start == range.start && reversed.end == range.end
                    && reversed.text == range.text,
            "Reversed source boundaries did not normalize");
    const auto geometry = model.annotationGeometry(range.start, range.end);
    const auto reversedGeometry = model.annotationGeometry(range.end, range.start);
    require(!geometry.vertical && !geometry.rectangles.isEmpty() && geometry.start.visible
                    && geometry.end.visible,
            "Visible passage lost its logical endpoint geometry");
    require(geometry.start.outward == QPoint(-1, 0) && geometry.end.outward == QPoint(1, 0),
            "LTR source edges face inside the selected passage");
    require(geometry.start.first.x() < geometry.end.first.x()
                    && geometry.start.first.x() == geometry.start.last.x()
                    && geometry.end.first.x() == geometry.end.last.x(),
            "LTR endpoint spans do not follow the source glyph edges");
    require(reversedGeometry.rectangles == geometry.rectangles
                    && reversedGeometry.start.first == geometry.start.first
                    && reversedGeometry.start.last == geometry.start.last
                    && reversedGeometry.end.first == geometry.end.first
                    && reversedGeometry.end.last == geometry.end.last,
            "Reversing source anchors swapped or moved normalized endpoint markers");
    require(model.visibleRanges({ { range.start, range.end },
                                  { range.end, range.start },
                                  { "bad", range.end } })
                    == QVector<int>({ 0, 1 }),
            "Visible source filtering lost reverse ranges or accepted malformed anchors");
    const auto invalidGeometry = model.annotationGeometry("bad", range.end);
    require(invalidGeometry.rectangles.isEmpty() && !invalidGeometry.start.visible
                    && !invalidGeometry.end.visible,
            "Malformed source anchor produced an endpoint marker");
    const auto crossed = model.annotationRange(last.start, first.end);
    require(crossed.text == range.text, "Crossed word boundaries excluded their words");
    const auto inlineFirst = selected.value("rainbow"), inlineLast = selected.value("cooperation");
    const auto inlineRange = model.textRange(inlineFirst.start, inlineLast.end);
    require(model.annotationRange(inlineLast.start, inlineFirst.end).text == inlineRange.text,
            "Crossed inline markup boundaries changed segmentation");
    const auto hanFirst = selected.value(QString::fromUtf8("日")),
               hanLast = selected.value(QString::fromUtf8("本"));
    require(model.annotationRange(hanLast.start, hanFirst.end).text
                    == model.textRange(hanFirst.start, hanLast.end).text,
            "Crossed Han boundaries changed segmentation");
    const QVector<QPair<QString, QString>> overlapping = {
        { first.start, last.end }, { last.start, last.end },     { first.start, first.end },
        { last.end, last.start },  { first.start, first.start }, { "invalid", last.end }
    };
    require(model.intersectingRanges(last.start, last.end, overlapping) == QVector<int>({ 0, 1 }),
            "Word intersection lost overlap IDs, accepted invalid ranges or treated an exclusive end as text");
    require(model.intersectingRanges(first.start, first.end, overlapping) == QVector<int>({ 0, 2 }),
            "First word did not intersect saved passage");
    require(model.intersectingRanges(last.end, first.start, overlapping).isEmpty()
                    && model.intersectingRanges("invalid", last.end, overlapping).isEmpty(),
            "Invalid tapped word produced passage candidates");
    require(model.intersectingRanges(inlineFirst.start, inlineFirst.end,
                                     { { inlineRange.start, inlineRange.end } })
                    == QVector<int>({ 0 }),
            "Inline markup word did not intersect saved passage");
    require(model.intersectingRanges(
                    hanLast.start, hanLast.end,
                    { { hanFirst.start, hanLast.end }, { hanFirst.start, hanFirst.end } })
                    == QVector<int>({ 0 }),
            "Adjacent Han word matched an exclusive saved endpoint");
    require(model.textRange("bad", last.end).text.isEmpty(), "Invalid source anchor selected text");
    require(model.textRange(first.start, first.start).text.isEmpty(), "Empty range selected text");
    auto tokenFor = [&](const QString &query) {
        QVector<kobalt::SearchHit> hits;
        for (int page = 0; page < model.pages() && hits.isEmpty(); ++page) {
            hits = model.searchPage(query, page).hits;
            if (!hits.isEmpty()) {
                require(model.go(page), "Cannot navigate to punctuation fixture");
            }
        }
        if (hits.isEmpty()) {
            fprintf(stderr, "Missing punctuation query %s\n", qPrintable(query));
        }
        require(!hits.isEmpty(), "Punctuation fixture text is missing");
        const auto rects = model.rangeRects(hits.first().start, hits.first().end);
        require(!rects.isEmpty(), "Punctuation fixture geometry is missing");
        return model.annotationTokenAt(rects.first().center());
    };
    const auto quoted = tokenFor("quoted");
    require(quoted.text == "quoted" && !quoted.startBeforePunctuation.isEmpty()
                    && !quoted.endAfterPunctuation.isEmpty(),
            "Quoted word lost its exact base or optional punctuation boundaries");
    const auto quotation =
            model.textRange(quoted.startBeforePunctuation, quoted.endAfterPunctuation);
    const auto quoteGeometry = model.annotationGeometry(quotation.start, quotation.end);
    require(quoteGeometry.start.visible && quoteGeometry.end.visible
                    && !quoteGeometry.rectangles.isEmpty(),
            "Inline punctuation boundaries lost their source endpoint markers");
    require(quotation.text == QString::fromUtf8("“quoted…”"),
            "Punctuation edge included whitespace or next word");
    const auto bracket = tokenFor("bracket");
    require(bracket.text == "bracket"
                    && model.textRange(bracket.startBeforePunctuation, bracket.endAfterPunctuation)
                                    .text
                            == "[bracket],",
            "Bracket and comma endpoint anchors are wrong");
    const auto plainToken = tokenFor("Next");
    require(plainToken.text == "Next" && plainToken.startBeforePunctuation.isEmpty()
                    && plainToken.endAfterPunctuation.isEmpty(),
            "Word without punctuation offered punctuation boundaries");
    const auto supplementary = tokenFor(QString::fromUtf8("𐐀token"));
    require(supplementary.text == QString::fromUtf8("𐐀token")
                    && model.textRange(supplementary.start, supplementary.endAfterPunctuation).text
                            == QString::fromUtf8("𐐀token。"),
            "Supplementary word or non-Latin punctuation boundary changed");
    const auto periodHits = model.searchPage(".", 0).hits;
    require(!periodHits.isEmpty(), "No period fixture");
    const auto periodRects = model.rangeRects(periodHits.first().start, periodHits.first().end);
    require(!periodRects.isEmpty(), "No period geometry");
    const auto periodPoint = periodRects.first().center();
    const auto period = model.annotationTokenAt(periodPoint);
    require(period.text == "." && model.wordAt(periodPoint).text.isEmpty(),
            "Annotation period was not selectable independently of dictionary words");
    const auto ellipsis = tokenFor(QString::fromUtf8("…"));
    require(ellipsis.text == QString::fromUtf8("…”"),
            "Direct punctuation run was truncated or selected its word");
    const auto dictionaryQuote = model.wordAt(quoted.rectangles.first().center());
    require(dictionaryQuote.text == "quoted" && dictionaryQuote.start == quoted.start
                    && dictionaryQuote.end == quoted.end,
            "Annotation punctuation changed dictionary word boundaries");
    const auto reversedPunctuation =
            model.annotationRange(bracket.startBeforePunctuation, bracket.endAfterPunctuation,
                                  quoted.startBeforePunctuation, quoted.endAfterPunctuation);
    require(reversedPunctuation.text == QString::fromUtf8("“quoted…” [bracket],"),
            "Reversed explicit punctuation boundaries were dropped");
    require(model.annotationRange(bracket.start, bracket.end, quoted.start, quoted.end).text
                    == QString::fromUtf8("quoted…” [bracket"),
            "Ordinary reversed word choice silently included outer punctuation");
    require(model.annotationRange(quoted.start, quoted.start, bracket.end, bracket.end).text
                    == model.textRange(quoted.start, bracket.end).text,
            "Saved endpoint tokens did not resolve precisely");
    require(model.annotationRange(period.start, period.end, quoted.start, quoted.end).text
                    == model.textRange(period.start, quoted.end).text,
            "Reverse punctuation token range changed source boundaries");
    const auto alpha = tokenFor("Alpha"), clearly = tokenFor("clearly"),
               finishes = tokenFor("finishes");
    const auto fully = tokenFor("fully");
    const auto sentence =
            model.annotationRange(alpha.start, alpha.end, clearly.start, clearly.end, true, true);
    QElapsedTimer resolverTimer;
    resolverTimer.start();
    for (int n = 0; n < 100; ++n) {
        require(model.annotationRange(alpha.start, alpha.end, clearly.start, clearly.end, true,
                                      true)
                                .text
                        == "Alpha finishes clearly.",
                "Repeated sentence resolver changed its result");
    }
    printf("Host sentence resolver: 100 calls in %lld microseconds\n",
           static_cast<long long>(resolverTimer.nsecsElapsed() / 1000));
    require(sentence.text == "Alpha finishes clearly." && sentence.chosenEnd == clearly.end
                    && sentence.sentenceEndPolicy,
            "Complete sentence did not derive punctuation while preserving chosen endpoint");
    require(model.annotationRange(finishes.start, finishes.end, clearly.start, clearly.end, true,
                                  true)
                            .text
                    == "finishes clearly",
            "Partial sentence ending at final word gained punctuation");
    require(model.annotationRange(finishes.start, finishes.end, fully.start, fully.end, true, true)
                    .text.endsWith("fully!"),
            "Complete final sentence did not control multi-sentence punctuation");
    require(model.annotationRange(clearly.start, clearly.end, alpha.start, alpha.end, true, true)
                            .text
                    == sentence.text,
            "Reverse token selection changed automatic sentence punctuation");
    require(model.annotationRange(alpha.start, alpha.end, clearly.start, clearly.end).text
                    == "Alpha finishes clearly",
            "Legacy exact range gained automatic punctuation");
    require(model.annotationRange(finishes.start, finishes.start, sentence.chosenEnd,
                                  sentence.chosenEnd, true)
                            .text
                    == "finishes clearly",
            "Moving saved start inward retained previously automatic punctuation");
    const auto quoteStart = tokenFor("Quotation"), quoteEnd = tokenFor("ending");
    require(model.annotationRange(quoteStart.start, quoteStart.end, quoteEnd.start, quoteEnd.end,
                                  true, false)
                            .text
                    == "Quotation ending.",
            "Unselected opener incorrectly added closing quote or suppressed period");
    require(model.annotationRange(quoteStart.startBeforePunctuation, quoteStart.end, quoteEnd.start,
                                  quoteEnd.end, true, true)
                            .text
                    == QString::fromUtf8("“Quotation ending.”"),
            "Selected matching opening quote did not include closing quote");
    const auto bracketStart = tokenFor("Nested"), bracketEnd = tokenFor("closing");
    require(model.annotationRange(bracketStart.startBeforePunctuation, bracketStart.end,
                                  bracketEnd.start, bracketEnd.end, true, true)
                            .text
                    == "(Nested closing?)",
            "Selected matching opening bracket did not include closing bracket");
    const auto earlier = tokenFor("Earlier"), rests = tokenFor("rests");
    require(model.annotationRange(earlier.startBeforePunctuation, earlier.end, rests.start,
                                  rests.end, true, true)
                    .text.endsWith("Later rests."),
            "Earlier completed quote supplied an unmatched opener to final sentence");
    const auto contraction = tokenFor("We're"), arriving = tokenFor("arriving");
    require(model.annotationRange(contraction.start, contraction.end, arriving.start, arriving.end,
                                  true, true)
                            .text
                    == "We're arriving.",
            "Contraction apostrophe was treated as an opening quote");
    const auto automaticQuote = model.annotationRange(quoteStart.start, quoteStart.end,
                                                      quoteEnd.start, quoteEnd.end, true, true);
    require(automaticQuote.text == QString::fromUtf8("“Quotation ending.”")
                    && automaticQuote.chosenStart == quoteStart.start
                    && automaticQuote.chosenEnd == quoteEnd.end
                    && automaticQuote.sentenceStartPolicy,
            "Ordinary word boundaries did not derive balanced quote punctuation");
    const auto nestedStart = tokenFor("Layered"), nestedEnd = tokenFor("pairing");
    const auto automaticNested = model.annotationRange(nestedStart.start, nestedStart.end,
                                                       nestedEnd.start, nestedEnd.end, true, true);
    require(automaticNested.text == QString::fromUtf8("(“Layered pairing.”)"),
            "Inline-separated nested opening, terminal and closing punctuation did not derive together");
    require(model.annotationRange(nestedEnd.start, nestedEnd.end, nestedStart.start,
                                  nestedStart.end, true, true)
                            .text
                    == automaticNested.text,
            "Reversed selection changed automatic opening pair");
    const auto nestedFragment = model.annotationRange(
            nestedStart.start, nestedStart.start, nestedStart.start, nestedStart.end, true, true);
    require(nestedFragment.text == "Layered" && nestedFragment.chosenStart == nestedStart.start
                    && nestedFragment.sentenceStartPolicy,
            "Moving end inward kept provisional opening punctuation or lost raw start eligibility");
    require(model.annotationRange(nestedFragment.chosenStart, nestedFragment.chosenStart,
                                  nestedEnd.start, nestedEnd.end, true,
                                  nestedFragment.sentenceStartPolicy)
                            .text
                    == automaticNested.text,
            "Extending a saved raw-start fragment did not restore complete opening pair");
    const auto greeting = tokenFor("Greeting"), murmured = tokenFor("murmured"),
               softly = tokenFor("softly");
    require(model.annotationRange(greeting.start, greeting.end, murmured.start, murmured.end, true,
                                  true)
                            .text
                    == QString::fromUtf8("Greeting,” she murmured"),
            "Closing quote inside an incomplete initial sentence falsely justified opening quote");
    require(model.annotationRange(greeting.start, greeting.end, softly.start, softly.end, true,
                                  true)
                            .text
                    == QString::fromUtf8("“Greeting,” she murmured softly."),
            "Complete reported-speech sentence did not restore its opening quote");
    const auto firstQuoteWord = tokenFor("First"), firstQuoteEnd = tokenFor("one"),
               finalQuoteEnd = tokenFor("two");
    require(model.annotationRange(firstQuoteWord.start, firstQuoteWord.end, finalQuoteEnd.start,
                                  finalQuoteEnd.end, true, true)
                            .text
                    == QString::fromUtf8("“First one. Second two.”"),
            "Complete quotation across sentences lost its automatic attached pair");
    require(model.annotationRange(firstQuoteWord.start, firstQuoteWord.end, firstQuoteEnd.start,
                                  firstQuoteEnd.end, true, true)
                            .text
                    == "First one.",
            "Partial multi-sentence quotation gained an unmatched opening quote");
    const auto doctor = tokenFor("Dr"), smith = tokenFor("Smith"), waits = tokenFor("waits");
    require(model.annotationRange(doctor.start, doctor.end, doctor.start, doctor.end, true, true)
                                    .text
                            == "Dr"
                    && model.annotationRange(smith.start, smith.end, waits.start, waits.end, true,
                                             true)
                                    .text
                            == "Smith waits",
            "Abbreviation was mistaken for complete sentence boundary");
    const auto remains = tokenFor("remains");
    require(model.annotationRange(remains.start, remains.end, remains.start, remains.end, true,
                                  true)
                            .text
                    == "remains",
            "Decimal period created a false sentence start");
    const auto japaneseStart = tokenFor(QString::fromUtf8("日")),
               japaneseEnd = tokenFor(QString::fromUtf8("葉"));
    require(model.annotationRange(japaneseStart.start, japaneseStart.end, japaneseEnd.start,
                                  japaneseEnd.end, true, true)
                            .text
                    == QString::fromUtf8("日本語の言葉。"),
            "Sentence completeness used Latin spaces instead of CJK token boundaries");
    const auto cityStart = tokenFor(QString::fromUtf8("東")),
               cityEnd = tokenFor(QString::fromUtf8("京"));
    require(model.annotationRange(cityStart.start, cityStart.end, cityEnd.start, cityEnd.end, true,
                                  true)
                            .text
                    == QString::fromUtf8("東京。"),
            "Distinct CJK sentence did not include terminal punctuation");
    const auto ends = tokenFor("Ends"), ellipsisWord = tokenFor("ellipsis");
    require(!model.annotationRange(ends.start, ends.end, ellipsisWord.start, ellipsisWord.end, true,
                                   true)
                     .text.endsWith(QString::fromUtf8("…")),
            "Ambiguous ellipsis expanded automatically");
    require(model.annotationRange(quoteStart.start, quoteStart.end, quoteEnd.start,
                                  quoteEnd.endAfterPunctuation)
                            .text
                    == QString::fromUtf8("Quotation ending.”"),
            "Manual punctuation override lost exact closing punctuation");
    require(model.go(0), "Cannot restore initial fixture page");
    const auto anchor = model.position();
    const auto crossStart = first.start;
    require(model.wordAt({ 0, 0 }).text.isEmpty(), "Margin selected a word");
    require(model.wordAt({ -1, 100 }).text.isEmpty(), "Outside viewport selected a word");
    require(model.go(model.pages() - 1), "Cannot navigate to the chapter boundary");
    const auto pageEnd = model.annotationPageEdges();
    require(!pageEnd.continuesBefore && !pageEnd.continuesAfter,
            "A separate short spine section must block selection turns at both ends");
    const int sectionPage = model.page();
    require(model.go(sectionPage - 1) && !model.annotationPageEdges().continuesAfter,
            "Selection must stop before the next spine section");
    require(model.go(1) && model.annotationPageEdges().continuesBefore
                    && model.annotationPageEdges().continuesAfter,
            "Ordinary paragraph boundaries must allow selection to continue across pages");
    require(model.go(sectionPage), "Cannot restore the section page");
    require(pageEnd.last.text == "again" && !pageEnd.last.punctuation,
            "Backward page extension must select the last word, not its trailing full stop");
    const auto wordEnd = model.annotationGeometry(pageEnd.last.start, pageEnd.last.end).end;
    require(QPoint::dotProduct(pageEnd.end.first - wordEnd.first, wordEnd.outward) > 0,
            "The physical page boundary must remain beyond the final punctuation");
    found.clear();
    for (int y = 0; y < 180; y += 10)
        for (int x = 0; x < 1072; x += 12) {
            const auto text = model.wordAt({ x, y }).text;
            if (!text.isEmpty())
                found.insert(text);
        }
    require(found.contains("Chapter") && found.contains("boundary"),
            "Selection used the old page or crossed the chapter boundary");
    kobalt::WordSelection boundary;
    for (int y = 0; y < 180 && boundary.text != "boundary"; y += 10) {
        for (int x = 0; x < 1072 && boundary.text != "boundary"; x += 12) {
            const auto word = model.wordAt({ x, y });
            if (word.text == "boundary") {
                boundary = word;
            }
        }
    }
    const auto cross = model.textRange(crossStart, boundary.end);
    require(cross.text.startsWith("Hello") && cross.text.endsWith("boundary"),
            "Cross-page passage was truncated");
    require(!model.rangeRects(cross.start, cross.end).isEmpty(),
            "Cross-page geometry missing on last page");
    const auto lastPageGeometry = model.annotationGeometry(cross.start, cross.end);
    require(!lastPageGeometry.rectangles.isEmpty() && !lastPageGeometry.start.visible
                    && lastPageGeometry.end.visible,
            "Last page repeated the off-page opening marker or lost the logical end");
    const QVector<QPair<QString, QString>> crossRanges = { { cross.start, cross.end },
                                                           { boundary.start, boundary.end } };
    require(model.intersectingRanges(boundary.start, boundary.end, crossRanges)
                    == QVector<int>({ 0, 1 }),
            "End-page word did not intersect cross-page passage");
    require(model.visibleRanges(crossRanges) == QVector<int>({ 0, 1 }),
            "Last page source filtering lost visible passage ranges");
    require(model.goTo(anchor), "Cannot restore first page");
    require(!model.rangeRects(cross.start, cross.end).isEmpty(),
            "Cross-page geometry missing on first page");
    require(model.visibleRanges(crossRanges) == QVector<int>({ 0 })
                    && model.visibleRanges({ { anchor, cross.end } }) == QVector<int>({ 0 }),
            "Source filtering resolved an off-page range or rejected a page bookmark");
    const auto firstPageGeometry = model.annotationGeometry(cross.start, cross.end);
    require(!firstPageGeometry.rectangles.isEmpty() && firstPageGeometry.start.visible
                    && !firstPageGeometry.end.visible,
            "First page repeated the off-page closing marker or lost the logical start");
    require(model.pages() > 2 && model.go(1), "Cross-page fixture has no middle page");
    require(model.visibleRanges(crossRanges) == QVector<int>({ 0 }),
            "Continuation page source filtering lost the spanning range");
    const auto middleGeometry = model.annotationGeometry(cross.start, cross.end);
    require(!middleGeometry.vertical && !middleGeometry.rectangles.isEmpty()
                    && !middleGeometry.start.visible && !middleGeometry.end.visible,
            "Continuation page fabricated source endpoints from its visible fragment");
    require(model.applyAppearance(kobalt::qt::object({ { "font_pixels", 64 },
                                                       { "margin_pixels", 24 },
                                                       { "hyphenation", "on" } }),
                                  QSize(360, 700)),
            "Cannot reflow selection fixture");
    require(model.textRange(cross.start, cross.end).text == cross.text,
            "Reflow changed source passage");
    require(model.goTo(cross.start) && !model.rangeRects(cross.start, cross.end).isEmpty(),
            "Reflow lost passage geometry");
    const auto reflowGeometry = model.annotationGeometry(cross.start, cross.end);
    require(reflowGeometry.start.visible && !reflowGeometry.end.visible,
            "Reflow changed the logical opening endpoint or repeated an off-page end");
    require(model.intersectingRanges(boundary.start, boundary.end, crossRanges)
                            == QVector<int>({ 0, 1 })
                    && model.intersectingRanges(first.start, first.end, crossRanges)
                            == QVector<int>({ 0 }),
            "Reflow changed passage membership or stable candidate order");
    require(model.textRange(quotation.start, quotation.end).text == quotation.text
                    && model.annotationRange(
                                    bracket.startBeforePunctuation, bracket.endAfterPunctuation,
                                    quoted.startBeforePunctuation, quoted.endAfterPunctuation)
                                    .text
                            == reversedPunctuation.text,
            "Reflow changed explicit punctuation ranges");
    require(model.annotationRange(alpha.start, alpha.end, clearly.start, clearly.end, true, true)
                            .text
                    == sentence.text,
            "Reflow changed automatic sentence punctuation");
    require(model.annotationRange(nestedStart.start, nestedStart.end, nestedEnd.start,
                                  nestedEnd.end, true, true)
                            .text
                    == automaticNested.text,
            "Reflow changed automatic nested opening pair");
    bool wrapped = false;
    for (int page = 0; page < qMin(6, model.pages()) && !wrapped; ++page) {
        model.go(page);
        for (int y = 0; y < 700 && !wrapped; y += 12)
            for (int x = 24; x < 336 && !wrapped; x += 12) {
                const auto word = model.wordAt({ x, y });
                wrapped = word.text == "extraordinary" && word.rectangles.size() >= 2;
            }
    }
    require(wrapped,
            "Wrapped discretionary hyphen did not return the complete word with two line rectangles");
    puts("PASS: layout hit testing, punctuation, apostrophes, combining text, inline spans, soft hyphens and chapter navigation");
}
