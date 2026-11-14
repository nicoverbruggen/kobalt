#pragma once
#include "engine/types.h"
#include <QObject>
#include <functional>

namespace kobalt {
class ReaderSession;
struct Highlight;

// One selection owns its endpoints, resolved range and displayed geometry.
// Interactive queries follow the viewport generation. Save preparation has a
// separate revision so that hiding or suspending the reader does not cancel it.
class AnnotationDraft final : public QObject
{
public:
    struct Endpoints
    {
        AnnotationToken start, end;
        bool startPolicy = false, endPolicy = false;
    };

    using Ranges = QVector<QPair<QString, QString>>;

    struct Queries
    {
        std::function<void(Endpoints, std::function<void(TextRange)>)> range;
        std::function<void(QString, QString, Ranges, std::function<void(QVector<int>)>)> overlaps;
        std::function<void(QString, QString, std::function<void(AnnotationGeometry)>)> geometry;
    };
    enum class Prepared { Ready, Rejected, Stale };

    AnnotationDraft(ReaderSession &session, std::function<bool()> interactive);
    AnnotationDraft(Queries queries, std::function<bool()> interactive);

    const Endpoints &endpoints() const { return endpoints_; }

    const TextRange &range() const { return range_; }

    const QString &editingId() const { return editing_; }

    const AnnotationGeometry &geometry() const { return geometry_; }

    const QString &viewport() const { return viewport_; }

    bool dirty() const { return dirty_; }

    quint64 generation() const { return generation_; }

    quint64 saveRevision() const { return saveRevision_; }

    QString end() const;

    // Discarding or replacing a selection also abandons its save revision.
    void clear();
    void edit(const Highlight &highlight);

    void markDirty() { dirty_ = true; }

    void invalidate();
    bool setViewport(const QString &viewport);

    Endpoints moved(const AnnotationToken &token, int boundary) const;
    // Publish only after range and overlap checks. accepted runs before the
    // geometry request, letting the owner update its next render's markup.
    void resolve(Endpoints endpoints, Ranges excluded, std::function<void(bool reversed)> accepted,
                 std::function<void()> ready, std::function<void()> rejected);
    void refreshGeometry(std::function<void()> ready);
    void acceptFrame(const QString &start, const QString &end, AnnotationGeometry geometry,
                     bool unchangedViewport);

    // The fallback paint hides rectangles while its geometry query is pending.
    void hideRectangles() { geometry_.rectangles.clear(); }

    void prepareSave(std::function<void(Prepared, quint64)> ready);
    // A failed save retains the draft; a stale completion cannot clear a newer one.
    bool finishSave(quint64 revision, bool saved);

private:
    Queries queries_;
    std::function<bool()> interactive_;
    Endpoints endpoints_;
    TextRange range_;
    QString editing_, viewport_;
    AnnotationGeometry geometry_;
    bool dirty_ = false;
    quint64 generation_ = 0, query_ = 0, geometryQuery_ = 0, saveRevision_ = 0;

    bool current(quint64 generation, quint64 query) const;
    bool apply(Endpoints endpoints, TextRange range);
};
} // namespace kobalt
