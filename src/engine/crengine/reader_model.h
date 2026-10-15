#pragma once
#include "publication/opening_result.h"
#include "reading/chapter.h"
#include "assets/font_catalog.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QMap>
#include <QPair>
#include <QRect>
#include "publication/resource_cache.h"
#include "platform/api/position_codec.h"
#include "reader_links.h"
#include "reader_word_counts.h"
#include "reading/reading_density.h"
#include "reading/appearance_settings.h"
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>
#include <functional>
#include <utility>

class LVDocView;
namespace kobalt {
namespace nickel {
class NickelPositionCodec;
}
// Fixed sans-serif fallback for publisher books without embedded fonts.
inline constexpr auto publisherSansFontFace = "NV Legible Next";
// Bundled face for the generic monospace family.
inline constexpr auto monospaceFontFace = "IBM Plex Mono";
// Bundled faces, in order, for characters that the reading font lacks.
// The CJK face has full coverage and localized glyphs selected by HarfBuzz
// from the book or element language. Untagged text uses its Japanese forms.
inline constexpr auto fallbackFontFaces =
    "Noto Serif|Noto Sans Math|Noto Serif CJK JP|Noto Naskh Arabic|Noto Serif Hebrew|Noto Serif Armenian|Noto Serif Georgian|Noto Serif Devanagari|Noto Serif Bengali|Noto Serif Tamil|Noto Serif Thai";
struct ReaderSource;
enum class OpeningStage { Content, Text, Styles, Pages, Ready };
using OpeningProgress = std::function<void(OpeningStage, int)>;
struct ChapterProgress {
    QString title;
    int page = 0, pages = 0;
};
struct SearchHit {
    QString start, end, before, text, after;
    int page = -1;
};
struct SearchBatch {
    QVector<SearchHit> hits;
    int nextPage = 0;
    bool done = false, limited = false;
};
struct WordSelection {
    QString text, start, end;
    QVector<QRect> rectangles;
};

// All engine calls stay on one owning thread. Images own their pixels.
class ReaderModel {
public:
    explicit ReaderModel(EngineFontInputs fonts);
    ~ReaderModel();
    bool initializeFonts();
    QStringList fontFaces();
    QVector<ReadingFont> fontCatalog();
    const EngineFontInputs &fontInputs() const {
        return fonts_;
    }
    static bool supportedArchive(const QByteArray &bytes, QString *error);
    bool open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error, OpeningProgress progress = {});
    bool openResources(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes, QSize size,
                       const QJsonObject &saved, QString *error, OpeningProgress progress = {},
                       OpeningResult *result = nullptr);
    bool rebindResources(const std::shared_ptr<ResourceCache> &resources, const QMap<QString, qint64> &sizes);
    // Set before opening. CREngine keeps parsed books and their pages in
    // `directory`, keyed by the publication fingerprint and by `policy`, which
    // changes with the code that builds the parsed book. An empty directory
    // or fingerprint opens without the cache.
    void setDocumentCache(QString directory, QByteArray fingerprint, QByteArray policy, qint64 bytes);
    // Set before opening. Previews inherit these bytes from their source model.
    void setUserStyleSheet(QByteArray css) {
        userStyleSheet_ = std::move(css);
    }
    const QByteArray &userStyleSheet() const {
        return userStyleSheet_;
    }
    // The last opening restored the parsed book from the cache without
    // reading its text from the publication.
    bool openedFromCache() const {
        return openedFromCache_;
    }
    // Writes the parsed book and its pages to the cache, for the next opening.
    bool saveDocumentCache();
    // Shared count, on the owning engine thread. Populates the Stats graph
    // from source anchors. Counts visible source text once, including
    // non-linear spine fragments; excludes SVG and ruby annotation text.
    // Cancellation discards the result. Completed counts use document-cache
    // properties, so reflow and cached reopening do not scan text again.
    DocumentWordCounts wordCounts(std::function<bool()> current = {});
    qint64 totalWords() const {
        return totalWords_;
    }
    QString resourceError() const;
    QJsonObject resourceStats() const;
    bool openPreview(ReaderModel &source, QSize size, QString *error);
    bool go(int page);
    // The page that `delta` page turns from `from` reach. When hiding is enabled,
    // skip other non-linear sections. Returns `from` at either end.
    int turnTarget(int from, int delta) const;
    bool goTo(const QString &position);
    QString platformPosition(const PositionCodec &codec, QString *error) const;
    bool goToPlatform(const PositionCodec &codec, const QString &location, QString *error);
    // Where goToPlatformNearest opened: the saved position, the start of its
    // chapter or the start of the book.
    enum class PositionTarget { Exact, Chapter, Book, Unavailable };
    // With a codec, an unknown anchor opens its chapter and an unknown chapter
    // opens the book. Without one, leave the position unchanged and report
    // Unavailable. *error explains why the exact position was not used.
    PositionTarget goToPlatformNearest(const PositionCodec &codec, const QString &location, QString *error);
    // Negative is before the current source anchor; positive is beyond it.
    int comparePlatformPosition(const PositionCodec &codec, const QString &location, QString *error) const;
    // The page of each position in the current layout, or -1 where a
    // position no longer resolves.
    QVector<int> pagesOf(const QStringList &positions) const;
    // The start position of a page and its first words.
    QString pageStart(int page) const;
    QString pageExcerpt(int page) const;
    WordSelection wordAt(QPoint point);
    QVector<PageLink> pageLinks();
    LinkDetails linkDetails(const QString &source);
    SearchBatch searchPage(const QString &query, int page);
    QVector<QRect> rangeRects(const QString &start, const QString &end);
    bool applyAppearance(const QJsonObject &settings, QSize size = {}, OpeningProgress progress = {});
    bool style(int pixels, int spacing);
    bool style(int pixels, int spacing, const QString &face, int margin);
    bool style(int pixels, int spacing, const QString &face, int margin, const QString &alignment);
    bool setWeight(int steps);
    bool setThickness(int steps);
    bool setHyphenation(const QString &mode);
    void setDarkMode(bool enabled);
    void resize(QSize size);
    // TODO (later): Add text selection and persistent colored highlights.
    // First investigate Kobo's highlight ranges, colors, markup and note
    // storage so existing highlights can be imported and survive reflow.
    // Written annotations are out of scope; prioritize basic reading first.
    QImage image(int page);
    bool loadNextImage();
    // The idle retained book keeps text and pagination, not cached pictures.
    void releaseImages();
    // Compares the cached images of two pages ahead of a turn between them.
    // Cache key of a page's cached image, or 0. Never draws.
    qint64 cachedImageKey(int page) const {
        return cache_.value(page).cacheKey();
    }
    void prepareDamage(int fromPage, int toPage);
    // The prepared rectangle for these two images, or false if there is none.
    bool preparedDamage(qint64 fromKey, qint64 toKey, QRect *damage) const;
    int page() const;
    int pages() const;
    int fontSize() const {
        return pixels_;
    }
    int spacing() const {
        return spacing_;
    }
    int margin() const {
        return margin_;
    }
    FontWeightAxis weightAxis() const;
    FontOpticalSizeAxis opticalSizeAxis() const;
    double opticalSize() const {
        return opticalSize_;
    }
    bool setOpticalSize(double points);
    int thickness() const {
        return weight_;
    }
    int weight() const {
        return weightAxis().available() ? axisWeight_ : weight_;
    }
    QString fontFace() const {
        return face_;
    }
    int embeddedFontCount() const;
    // The face CREngine uses for the text at the reading position. With publisher
    // fonts this is the book's own face, which may be embedded in the book.
    QString textFontFace() const;
    QString alignment() const {
        return alignment_;
    }
    QSize viewport() const {
        return size_;
    }
    int cachedPages() const {
        return cache_.size();
    }
    QString position() const;
    QString title() const;
    QString author() const;
    QImage coverThumbnail() const;
    QVector<ChapterLink> chapters() const;
    void setChapters(QVector<ChapterLink> contents, QVector<ChapterLink> graph);
    QVector<ChapterLink> chapterGraph() const;
    ChapterSelection chapterSelection(const QVector<ChapterLink> &links) const;
    ChapterProgress chapterProgress() const;
    // The title of the chapter that contains `position`, chosen like the
    // header's chapter. Empty when the book has no usable contents.
    QString chapterTitle(const QString &position) const;
    QJsonObject state() const;

private:
    // Sets the current and next entries of `links` around `position`.
    ChapterSelection chapterAround(const QVector<ChapterLink> &links, const QString &position) const;
    void markKoboLinkDecorations();
    EngineFontInputs fonts_;
    friend class nickel::NickelPositionCodec;
    std::shared_ptr<ReaderSource> source_;
    QJsonObject openingTimes_;
    std::unique_ptr<LVDocView> engine_;
    QSize size_;
    QString anchor_;
    QString nativeLocation_;
    QString cacheDirectory_;
    QByteArray cacheFingerprint_, cachePolicy_;
    QByteArray userStyleSheet_;
    QByteArray documentCachePolicy() const;
    qint64 cacheBytes_ = 0;
    bool openedFromCache_ = false;
    QVector<ChapterLink> contents_, graph_;
    qint64 totalWords_ = -1;
    QVector<ChapterLink> chapterPages(QVector<ChapterLink> links) const;
    QVector<ChapterLink> tocChapters() const;
    // Chapter entries resolved for one pagination: their pages, and the
    // entries with a page sorted by source position. Resolving every entry
    // again on each page turn delays the page by far more than drawing it.
    struct ChapterIndex {
        quint64 layout = 0;
        bool toc = false;
        QVector<ChapterLink> source, links;
        // Indexes into `links`, in source order. Equal positions keep list order.
        QVector<int> order;
        // CREngine places an element position with a child offset before every
        // position inside that element, and an earlier child can also compare
        // before it. Such a list has no consistent order, so it is scanned.
        bool sorted = false;
    };
    mutable ChapterIndex contentsIndex_, graphIndex_;
    // Changes whenever the pages change, including a layout that CREngine
    // redoes by itself.
    quint64 layoutSignature() const;
    const ChapterIndex &chapterIndex(ChapterIndex &index, const QVector<ChapterLink> &source, bool toc) const;
    QString face_ = defaultFontFace;
    QString defaultFace_ = defaultFontFace;
    QString alignment_ = "left";
    int pixels_ = defaultFontPixels(), spacing_ = defaultLineSpacing;
    int margin_ = defaultMarginPixels(), topMargin_ = defaultVerticalMarginPixels(margin_),
        bottomMargin_ = defaultVerticalMarginPixels(margin_);
    void activateFontParameters() const;
    int axisWeight_ = defaultVariableFontWeight;
    double opticalSize_ = 0;
    int weight_ = 0;
    QString bionic_ = "off";
    // The engine DOM version that parses this book and writes its positions.
    int domVersion_ = 0;
    QString hyphenation_ = "off";
    bool darkMode_ = false;
    bool smoothImages_ = true, hideNonlinearFragments_ = false;
    QMap<int, QImage> cache_;
    // One retired page, owned only by this model. Never retain client images.
    QImage spareImage_;
    quint64 imageRevision_ = 0;
    bool refreshImages();
    // Changed rectangle between two cached page images, by sorted cache keys.
    QMap<QPair<qint64, qint64>, QRect> damage_;
    bool openSource(std::shared_ptr<ReaderSource> source, QSize size, const QJsonObject &saved, QString *error,
                    OpeningProgress progress = {}, OpeningResult *result = nullptr);
    void trim();
    void applyPublisherFontFallbacks();
    void applyStyleSheet();
    void configure(QSize size, const QJsonObject &saved);
};
}
