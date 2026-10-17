#pragma once
#include "engine/crengine/reader_model.h"
#include "open_request.h"
#include <QElapsedTimer>
#include <QObject>
#include <atomic>
#include <functional>
#include <memory>

namespace kobalt {
struct ReaderFrame {
    int turnDirection = 0;
    QImage image;
    // Changed rectangle against the image with cache key `damageFrom`, if the
    // worker prepared it before this turn. `damageFrom` is 0 otherwise.
    QRect damage;
    qint64 damageFrom = 0;
    QJsonObject state, resources;
    QString title, author, error;
    qint64 totalWords = -1;
    QString nativePosition, nativePositionError;
    QImage cover;
    QStringList fonts;
    QVector<ReadingFont> fontCatalog;
    QVector<ChapterLink> chapters, chapterGraph;
    QVector<PageLink> links;
    ChapterSelection contentsSelection, graphSelection;
};

enum class RenderReason { Appearance, EnableBionic, DisableBionic, PageLayout };

struct RenderProgress {
    bool active = false;
    quint64 revision = 0;
    RenderReason reason = RenderReason::Appearance;
    OpeningStage stage = OpeningStage::Styles;
    int percent = 0;
    qint64 elapsedMilliseconds = 0;
    // Reflow styles and pagination share one bar; a phase change never empties it.
    int overallPercent() const {
        const int phase = qBound(0, int(stage) - int(OpeningStage::Styles), 2);
        return qMin(100, (phase * 100 + qBound(0, percent, 100)) / 2);
    }
};

// Where a platform position lies relative to the current page.
struct PositionComparison {
    enum class Status {
        Compared, // direction is valid
        Failed,   // the position cannot be placed; error says why
        Stale     // the book moved or changed layout while comparing
    };
    Status status = Status::Failed;
    // Compared only: negative before, zero at, positive after the current position.
    int direction = 0;
    QString error;
};

// GUI-side settings and immutable frame copies. The worker owns engine access.
// After application shutdown joins that worker, remaining models are released
// synchronously by their GUI-side owners.
class ReaderSession : public QObject {
public:
    ReaderSession(EngineFontInputs fonts, QString placeholderFamily);
    const EngineFontInputs &fontInputs() const {
        return fonts_;
    }
    const QString &placeholderFamily() const {
        return placeholderFamily_;
    }
    ~ReaderSession() override;
    // Call after deleting sessions and cancelling their resources.
    static void drain(std::function<void()> finished);
    void open(OpenRequest request, std::function<void(OpeningResult)> finished, OpeningProgress progress = {});
    // A fresh facade prevents callbacks from the closed reader reaching a reopen.
    std::unique_ptr<ReaderSession> retained();
    void cancelSource();
    void revokeSource();
    QByteArray sourceHash() const {
        return fingerprint;
    }
    // Whether the saved state passed to open belongs to this version of the book.
    bool savedStateMatched() const {
        return savedMatched;
    }
    QJsonObject resourceStats() const {
        return frame.resources;
    }
    void openPreview(const ReaderSession &source, QSize size, std::function<void(QString)> finished);
    void apply(const QJsonObject &settings, QSize size = {});
    std::function<void()> updated;
    RenderProgress renderProgress() const;
    // Appearance and viewport work reports progress separately from frames.
    // Each revision starts a new elapsed time, including its wait for the worker.
    std::function<void()> renderProgressChanged;
    // A buffered request reached the first or final page without moving.
    std::function<void(int direction)> turnBoundary;
    bool style(int pixels, int spacing);
    bool style(int pixels, int spacing, const QString &face, int margin);
    bool style(int pixels, int spacing, const QString &face, int margin, const QString &alignment);
    bool setWeight(int steps);
    bool setThickness(int steps);
    void setDarkMode(bool enabled);
    bool setHyphenation(const QString &mode);
    // Keep at most one turn behind the current turn. The latest direction wins.
    bool turn(int direction);
    bool canTurn() const;
    void cancelPendingTurn();
    bool go(int page);
    bool goTo(const QString &anchor);
    bool goToNative(const QString &location, std::function<void(QString)> finished);
    void nativePosition(std::function<void(QString, QString)> finished);
    void compareNativePosition(const QString &location, std::function<void(PositionComparison)> finished);
    // Bookmark lookups on the engine thread. They report nothing when the
    // layout changed before they finished; the caller asks again.
    void bookmarkPages(QStringList positions, std::function<void(QVector<int>)> finished);
    void pageSummary(int page, std::function<void(QString position, QString excerpt)> finished);
    void wordAt(QPoint point, std::function<void(WordSelection)> finished);
    void linkDetails(const QString &source, std::function<void(LinkDetails)> finished);
    const QVector<PageLink> &pageLinks() const {
        return frame.links;
    }
    void search(const QString &query, std::function<void(SearchBatch)> finished);
    void cancelSearch();
    void rangeRects(const QString &start, const QString &end, std::function<void(QVector<QRect>)> finished);
    void resize(QSize size);
    bool busy() const {
        return running;
    }
    // Preview and later reading-command errors. Initial opening failures are
    // delivered only through the typed completion result.
    QString error() const {
        return frame.error;
    }
    bool formattingWarning() const {
        return formattingWarning_;
    }
    QString imageWarning() const {
        return frame.resources.value("image_error").toString();
    }
    int turnDirection() const {
        return frame.turnDirection;
    }
    QRect damage() const {
        return frame.damage;
    }
    qint64 damageFrom() const {
        return frame.damageFrom;
    }
    QImage image() const {
        return frame.image;
    }
    QJsonObject state() const {
        return frame.state;
    }
    // The newest requested settings, including any whose frame is still rendering.
    QJsonObject requestedState() const {
        return desired;
    }
    bool temporaryTypography() const {
        return !temporaryAppearance.isEmpty();
    }
    QJsonObject persistentState() const;
    // Captured with the displayed frame, so sleep never waits for the engine worker.
    QString savedNativePosition(QString *error) const {
        *error = frame.nativePositionError;
        return frame.nativePosition;
    }
    int page() const {
        return frame.state.value("page").toInt();
    }
    int pages() const {
        return frame.state.value("pages").toInt();
    }
    // The page a forward turn reaches, or page() on the last page of the text.
    int nextPage() const {
        return frame.state.value("next_page").toInt(page());
    }
    int cachedPages() const {
        return frame.state.value("cached_pages").toInt();
    }
    int fontSize() const {
        return desired.value("font_pixels").toInt();
    }
    int spacing() const {
        return desired.value("line_spacing").toInt();
    }
    int margin() const {
        return desired.value("margin_pixels").toInt();
    }
    int topMargin() const {
        return desired.value("top_margin_pixels").toInt(defaultVerticalMarginPixels(margin()));
    }
    int bottomMargin() const {
        return desired.value("bottom_margin_pixels").toInt(defaultVerticalMarginPixels(margin()));
    }
    bool setVerticalMargins(int margin);
    FontOpticalSizeAxis opticalSizeAxis() const {
        return fontOpticalSizeAxis(frame.fontCatalog, fontFace());
    }
    double opticalSize() const {
        return normalizedOpticalSize(opticalSizeAxis(), desired.value("font_optical_size").toDouble());
    }
    bool setOpticalSize(double points);
    FontWeightAxis weightAxis() const {
        return fontWeightAxis(frame.fontCatalog, fontFace());
    }
    int thickness() const {
        return desired.value("weight_steps").toInt();
    }
    int weight() const {
        const auto axis = weightAxis();
        return axis.available()
                   ? qBound(axis.minimum, desired.value("font_weight").toInt(defaultVariableFontWeight), axis.maximum)
                   : desired.value("weight_steps").toInt();
    }
    QString fontFace() const {
        return desired.value("font_face").toString();
    }
    QString hyphenation() const {
        return desired.value("hyphenation").toString("off");
    }
    QString alignment() const {
        return desired.value("text_alignment").toString();
    }
    QString title() const {
        return frame.title;
    }
    QString author() const {
        return documentAuthor;
    }
    QImage coverThumbnail() const {
        return documentCover;
    }
    // Decodes on the engine worker. A cancelled or replaced source returns no image.
    void coverImage(QSize maximumSize, std::function<void(QImage)> finished, QString preferredPath = {});
    QString chapterTitle() const {
        return frame.state.value("chapter_title").toString();
    }
    int chapterPage() const {
        return frame.state.value("chapter_page").toInt();
    }
    int chapterPages() const {
        return frame.state.value("chapter_pages").toInt();
    }
    QStringList fontFaces() const {
        return frame.fonts;
    }
    QVector<ReadingFont> fontCatalog() const {
        return frame.fontCatalog;
    }
    QVector<ChapterLink> chapters() const {
        return frame.chapters;
    }
    QVector<ChapterLink> chapterGraph() const {
        return frame.chapterGraph;
    }
    ChapterSelection contentsSelection() const {
        return frame.contentsSelection;
    }
    ChapterSelection graphSelection() const {
        return frame.graphSelection;
    }
    qint64 totalWords() const {
        return frame.totalWords;
    }
    void setChapters(QVector<ChapterTarget> contents, QVector<ChapterTarget> graph);

private:
    bool formattingWarning_ = false;
    QJsonObject formattingDetails_;
    ReaderSession(std::shared_ptr<ReaderModel> retainedModel, QString placeholderFamily);
    EngineFontInputs fonts_;
    QString placeholderFamily_;
    std::shared_ptr<ReaderModel> model;
    ReaderFrame frame;
    QString documentAuthor;
    QImage documentCover;
    OpeningResult openingResult_;
    QByteArray fingerprint;
    bool savedMatched = false;
    std::shared_ptr<SourceLease> lease_;
    bool ownsLease_ = false;
    QString identity_;
    PositionCodec positions_;
    QJsonObject desired;
    QJsonObject temporaryAppearance, originalAppearance;
    QSize desiredSize;
    QString destination;
    int pageDelta = 0;
    int pageTarget = -1;
    bool running = false;
    bool turning = false;
    int pendingTurn = 0;
    bool loadingImage = false;
    bool preview_ = false;
    bool retiring = false;
    RenderProgress renderProgress_;
    QElapsedTimer renderTimer;
    void beginRenderProgress(RenderReason reason);
    void clearRenderProgress();
    void loadImages();
    void resumeIdleWork(int direction, quint64 job);
    quint64 revision = 0;
    // The newest request number, readable on the engine worker. Work that only
    // prepares later page turns stops when it no longer matches its own job.
    std::shared_ptr<std::atomic<quint64>> newest = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<std::atomic_bool> searchCancelled;
    void searchNext(QString query, int page, int count, quint64 job, std::shared_ptr<std::atomic_bool> cancelled,
                    std::function<void(SearchBatch)> finished);
    void request(bool appearanceChange = false, RenderReason reason = RenderReason::Appearance);
    void dispatch(bool pageTurn = false);
};
}
