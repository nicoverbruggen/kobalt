#include "column_view.h"
#include "engine_document.h"
#include "platform/api/position_codec.h"
#include "support/strings.h"
#include "engine_activity.h"
#include <lvdocview.h>

namespace kobalt {
namespace {
lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

QString decodePosition(const EngineDocument &engine, const PositionCodec &codec,
                       const QString &location, QString *error)
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
    go(0);
    return PositionTarget::Book;
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

QString EngineDocument::platformPosition(const PositionCodec &codec, QString *error) const
{
    if (codec)
        return codec.encode(*this, error);
    if (error)
        *error = strings::get("error.no_supported_native_reading_position");
    return {};
}

ResolvedChapters resolveChapterTargets(const EngineDocument &engine, const PositionCodec &codec,
                                       const QVector<ChapterTarget> &contents,
                                       const QVector<ChapterTarget> &graph)
{
    if (!codec)
        return {};
    QMap<QString, QString> anchors;
    auto resolve = [&](const QVector<ChapterTarget> &targets) {
        QVector<ChapterLink> links;
        for (const auto &target : targets) {
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
