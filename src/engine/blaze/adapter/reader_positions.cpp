#include "column_view.h"
#include "reader_source.h"
#include "engine_document.h"
#include "platform/api/position_codec.h"
#include "support/strings.h"
#include "engine/engine_activity.h"
#include <lvdocview.h>

namespace kobalt {
namespace {
lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

QString decodePosition(const Engine &engine, const PositionCodec &codec, const QString &location,
                       QString *error)
{
    if (codec)
        return codec.decode(engine, location, error);
    if (error)
        *error = strings::get("error.no_supported_native_reading_position");
    return {};
}
} // namespace

bool EngineDocument::goToPlatform(const PositionCodec &codec, const QString &location,
                                  QString *error)
{
    const auto anchor = decodePosition(*this, codec, location, error);
    if (anchor.isEmpty())
        return false;
    if (!goTo(anchor)) {
        if (error)
            *error = strings::get("error.cannot_select_the_native_text_position");
        return false;
    }
    nativeLocation_ = location;
    return true;
}

EngineDocument::PositionTarget EngineDocument::goToPlatformNearest(const PositionCodec &codec,
                                                                   const QString &location,
                                                                   QString *error)
{
    if (goToPlatform(codec, location, error))
        return PositionTarget::Exact;
    if (!codec)
        return PositionTarget::Unavailable;
    // A location without a fragment names the start of its chapter.
    const auto chapter = codec.chapterStart(location);
    if (!chapter.isEmpty() && goToPlatform(codec, chapter, nullptr))
        return PositionTarget::Chapter;
    return goToFraction(0) ? PositionTarget::Book : PositionTarget::Unavailable;
}

int EngineDocument::comparePlatformPosition(const PositionCodec &codec, const QString &location,
                                            QString *error) const
{
    if (!engine_) {
        if (error)
            *error = strings::get("error.no_supported_native_reading_position");
        return 0;
    }
    const auto anchor = decodePosition(*this, codec, location, error);
    return anchor.isEmpty() ? 0 : comparePosition(anchor, error);
}

int EngineDocument::comparePosition(const QString &anchor, QString *error) const
{
    if (!engine_) {
        if (error)
            *error = strings::get("error.cannot_compare_the_reading_positions");
        return 0;
    }
    auto *doc = engine_->getDocument();
    const ldomXPointerEx current(doc->createXPointer(cre(anchor_)));
    const ldomXPointerEx incoming(doc->createXPointer(cre(anchor)));
    if (current.isNull() || incoming.isNull()) {
        if (error)
            *error = strings::get("error.cannot_compare_the_reading_positions");
        return 0;
    }
    return incoming.compare(current);
}

int EngineDocument::describePosition(const QString &position, QString *chapter, int *page,
                                     QString *error, const std::function<bool()> &current)
{
    const int item = spineItemIndex(position);
    const bool outside =
            windowFirst_ >= 0 && item >= 0 && (item < windowFirst_ || item > windowLast_);
    auto *document = outside ? documentFor(item, current) : this;
    if (!document || !document->engine_ || (current && !current())
        || document->engine_->getDocument()->createXPointer(cre(position)).isNull()) {
        *error = strings::get("error.cannot_compare_the_reading_positions");
        return 0;
    }
    *chapter = document->chapterTitle(position);
    const int number = bookPages({ position }, current).value(0, -1);
    *page = number > 0 ? number - 1 : -1;
    return outside ? (item < windowFirst_ ? -1 : 1) : comparePosition(position, error);
}

QString EngineDocument::platformPosition(const PositionCodec &codec, QString *error) const
{
    if (codec)
        return codec.encode(*this, error);
    if (error)
        *error = strings::get("error.no_supported_native_reading_position");
    return {};
}

ResolvedChapters resolveChapterTargets(const Engine &engine, const PositionCodec &codec,
                                       const QVector<ChapterTarget> &contents,
                                       const QVector<ChapterTarget> &graph)
{
    if (!codec)
        return {};
    QMap<QString, QString> anchors;
    auto resolve = [&](const QVector<ChapterTarget> &targets) {
        QVector<ChapterLink> links;
        for (const auto &target : targets) {
            if (links.size() >= maximumChapterEntries)
                break;
            engineActivity().checkpoint();
            if (!anchors.contains(target.location))
                anchors.insert(target.location, codec.decode(engine, target.location, nullptr));
            links.append({ target.title, anchors.value(target.location), -1, target.depth,
                           target.words });
        }
        return links;
    };
    auto resolvedContents = resolve(contents);
    return { std::move(resolvedContents), resolve(graph) };
}
} // namespace kobalt
