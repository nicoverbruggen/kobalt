#include "engine/engine_worker.h"
#include "stop_point.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include "engine_document.h"
#include "reader_source.h"
#include "kobo_styles.h"
#include "reader_render_callback.h"
#include "reader_alignment.h"
#include "reader_beautifier.h"
#include "reader_page_layout.h"
#include "document_cache.h"
#include "engine/engine_activity.h"
#include "publication/publication.h"
#include <atomic>
#include <lvdocview.h>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <textlang.h>
#include <QFile>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QUrl>
#include <QXmlStreamWriter>
#include <QtEndian>
#include <climits>
#if __has_include(<koblaze.h>)
#  include <koblaze.h>
#endif

namespace kobalt {
namespace {
QString qstring(const lString32 &s)
{
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}

lString32 cre(const QString &s)
{
    return Utf8ToUnicode(s.toUtf8().constData());
}

constexpr int maxBook = 64 * 1024 * 1024;

class ResourceStream : public LVNamedStream
{
public:
    ResourceStream(std::shared_ptr<ResourceCache> resources, QString name, qint64 length,
                   QSize dimensions = {})
        : resources(std::move(resources)),
          name(std::move(name)),
          length(length),
          dimensions(dimensions)
    {
        SetName(cre(this->name).c_str());
    }

    lvopen_mode_t GetMode() override { return LVOM_READ; }

    LVImageSource *CreateImageSource(ldomDocument *doc, ldomNode *node) override;

    lvsize_t GetSize() override
    {
        resolveLength();
        return lvsize_t(qMax(qint64(0), length));
    }

    lverror_t GetSize(lvsize_t *size) override
    {
        if (!resolveLength())
            return LVERR_FAIL;
        *size = lvsize_t(length);
        return LVERR_OK;
    }

    lverror_t Seek(lvoffset_t offset, lvseek_origin_t origin, lvpos_t *result) override
    {
        if (!resolveLength())
            return LVERR_FAIL;
        const qint64 base = origin == LVSEEK_SET ? 0 : origin == LVSEEK_CUR ? position : length;
        const qint64 next = base + offset;
        if ((origin != LVSEEK_SET && origin != LVSEEK_CUR && origin != LVSEEK_END) || next < 0
            || next > length)
            return LVERR_FAIL;
        position = next;
        if (result)
            *result = lvpos_t(next);
        return LVERR_OK;
    }

    lverror_t Read(void *buffer, lvsize_t count, lvsize_t *read) override
    {
        if (read)
            *read = 0;
        if (!resolveLength())
            return LVERR_FAIL;
        const auto result =
                resources->read(name, position, buffer, qMin(qint64(count), length - position));
        if (result < 0)
            return LVERR_FAIL;
        position += result;
        if (read)
            *read = lvsize_t(result);
        return LVERR_OK;
    }

    lverror_t SetSize(lvsize_t) override { return LVERR_FAIL; }

    lverror_t Write(const void *, lvsize_t, lvsize_t *written) override
    {
        if (written)
            *written = 0;
        return LVERR_FAIL;
    }

    bool Eof() override { return !resolveLength() || position == length; }

private:
    std::shared_ptr<ResourceCache> resources;
    QString name;
    qint64 length, position = 0;
    QSize dimensions;

    bool resolveLength()
    {
        if (length < 0)
            length = resources->size(name);
        return length >= 0;
    }
};

class DeferredImage : public LVImageSource
{
public:
    DeferredImage(std::shared_ptr<ResourceCache> resources, QString name, QSize size,
                  ldomDocument *doc, ldomNode *node)
        : resources(std::move(resources)), name(std::move(name)), size(size), doc(doc), node(node)
    {
    }

    ldomDocument *GetSourceDocument() override { return doc; }

    ldomNode *GetSourceNode() override { return node; }

    LVStream *GetSourceStream() override { return nullptr; }

    void Compact() override { }

    int GetWidth() const override { return size.width(); }

    int GetHeight() const override { return size.height(); }

    bool Decode(LVImageDecoderCallback *callback) override
    {
        // Layout asks for dimensions with no callback. Only a paint requests
        // the pixels, so images on unread pages remain deferred.
        if (!callback)
            return true;
        if (resources->imageReady(name)) {
            const auto length = resources->size(name);
            if (length < 0)
                return false;
            auto image = LVCreateStreamImageSource(
                    LVStreamRef(new ResourceStream(resources, name, length)), doc, node);
            if (image.isNull() || image->GetWidth() != size.width()
                || image->GetHeight() != size.height())
                return LVCreateDummyImageSource(node, size.width(), size.height())
                        ->Decode(callback);
            return image->Decode(callback);
        }
        // A neutral rectangle keeps the final image's dimensions. One row is
        // enough here; the scaler may still allocate its own bitmap.
        std::vector<lUInt32> row(size.width(), 0x00eeeeee);
        callback->OnStartDecode(this);
        for (int y = 0; y < size.height(); ++y)
            callback->OnLineDecoded(this, y, row.data());
        callback->OnEndDecode(this, false);
        return true;
    }

private:
    std::shared_ptr<ResourceCache> resources;
    QString name;
    QSize size;
    ldomDocument *doc;
    ldomNode *node;
};

LVImageSource *ResourceStream::CreateImageSource(ldomDocument *doc, ldomNode *node)
{
    return dimensions.isValid() ? new DeferredImage(resources, name, dimensions, doc, node)
                                : nullptr;
}

class ResourceContainer : public LVNamedContainer
{
public:
    ResourceContainer(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes)
        : resources(std::move(resources))
    {
        SetName(U"publication.epub");
        for (const auto &name : this->resources->names()) {
            auto *item = new LVCommonContainerItemInfo;
            item->SetItemInfo(cre(name), lvsize_t(sizes.value(name)), 0);
            Add(item);
        }
    }

    LVContainer *GetParentContainer() override { return nullptr; }

    lverror_t GetSize(lvsize_t *size) override
    {
        *size = GetObjectCount();
        return LVERR_OK;
    }

    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override
    {
        const auto key = qstring(lString32(name));
        if (mode != LVOM_READ || !resources->contains(key))
            return {};
        const auto dimensions = resources->deferredImageSize(key);
        if (dimensions.isValid())
            return LVStreamRef(new ResourceStream(resources, key, -1, dimensions));
        const auto length = resources->size(key);
        return length < 0 ? LVStreamRef() : LVStreamRef(new ResourceStream(resources, key, length));
    }

private:
    std::shared_ptr<ResourceCache> resources;
};

class ContentContainer : public LVContainer
{
public:
    explicit ContentContainer(LVContainerRef source, QSet<QString> skippedFonts = {})
        : source(source), skippedFonts_(std::move(skippedFonts))
    {
    }

    bool IsContainer() override { return true; }

    const lChar32 *GetName() override { return source->GetName(); }

    LVContainer *GetParentContainer() override { return source->GetParentContainer(); }

    lverror_t GetSize(lvsize_t *size) override { return source->GetSize(size); }

    int GetObjectCount() const override { return source->GetObjectCount(); }

    const LVContainerItemInfo *GetObjectInfo(int index) override
    {
        return source->GetObjectInfo(index);
    }

    const LVContainerItemInfo *GetObjectInfo(lString32 name) override
    {
        return source->GetObjectInfo(name);
    }

    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override
    {
        const auto path = qstring(lString32(name));
        if (!skippedFonts_.isEmpty()) {
            const auto normalized = QDir::cleanPath(path);
            if (skippedFonts_.contains(normalized.startsWith('/') ? normalized.mid(1) : normalized))
                return {};
        }
        auto stream = source->OpenStream(name, mode);
        const auto suffix = QFileInfo(path).suffix().toLower();
        // Bound the temporary chapter copy; leave large or non-text resources streamed.
        constexpr lvsize_t limit = 2 * 1024 * 1024;
        if (stream.isNull() || mode != LVOM_READ
            || !(suffix.isEmpty() || suffix == "html" || suffix == "xhtml" || suffix == "htm"
                 || suffix == "xml")
            || stream->GetSize() > limit) {
            return stream;
        }
        QByteArray bytes(int(stream->GetSize()), '\0');
        lvsize_t read = 0;
        if (stream->Read(bytes.data(), bytes.size(), &read) != LVERR_OK
            || read != lvsize_t(bytes.size())) {
            stream->SetPos(0);
            return stream;
        }
        auto cleaned = withoutKoboStyleHacks(bytes);
        if (cleaned == bytes) {
            stream->SetPos(0);
            return stream;
        }
        return LVCreateMemoryStream(cleaned.data(), cleaned.size(), true, LVOM_READ);
    }

protected:
    LVContainerRef source;

private:
    QSet<QString> skippedFonts_;
};

class SectionContainer : public ContentContainer
{
public:
    SectionContainer(LVContainerRef source, lString32 packagePath, QByteArray package)
        : ContentContainer(source), packagePath(packagePath), package(std::move(package))
    {
        packageInfo.SetItemInfo(packagePath, this->package.size(), 0);
    }

    const LVContainerItemInfo *GetObjectInfo(int index) override
    {
        const auto *item = source->GetObjectInfo(index);
        return item && lString32(item->GetName()) == packagePath ? &packageInfo : item;
    }

    const LVContainerItemInfo *GetObjectInfo(lString32 name) override
    {
        return name == packagePath ? &packageInfo : source->GetObjectInfo(name);
    }

    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override
    {
        if (mode != LVOM_READ) {
            return {};
        }
        if (lString32(name) == packagePath) {
            return LVCreateMemoryStream(package.data(), package.size(), true, LVOM_READ);
        }
        return source->OpenStream(name, mode);
    }

private:
    lString32 packagePath;
    QByteArray package;
    LVCommonContainerItemInfo packageInfo;
};

} // namespace

namespace {
// Experiment: chapter mode is on with KOBALT_CHAPTER_MODE=1 in the
// environment or, on a device, with a file named chapter-mode in the Kobalt
// folder. The file is looked for once.
std::atomic_int chapterModeOverride{ -1 };

bool chapterMode()
{
    const int override = chapterModeOverride.load();
    if (override >= 0)
        return override > 0;
    if (qgetenv("KOBALT_CHAPTER_MODE").toInt() > 0)
        return true;
#ifdef KOBALT_CONFIG_DIR
    static const bool marker = QFile::exists(QStringLiteral(KOBALT_CONFIG_DIR "/chapter-mode"));
    return marker;
#else
    return false;
#endif
}

// A number in the chapter-mode file sets the pages of one slice.
int chapterModePages()
{
#ifdef KOBALT_CONFIG_DIR
    static const int pages = [] {
        QFile file(QStringLiteral(KOBALT_CONFIG_DIR "/chapter-mode"));
        return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toInt() : 0;
    }();
    return qBound(0, pages, 500);
#else
    return 0;
#endif
}

} // namespace

int spineItemIndex(const QString &position)
{
    static const QRegularExpression fragment(QStringLiteral("/DocFragment(?:\\[(\\d+)\\])?"));
    const auto match = fragment.match(position);
    return match.hasMatch() ? qMax(1, match.captured(1).toInt()) - 1 : -1;
}

bool EngineDocument::supportedArchive(const QByteArray &bytes, QString *error,
                                      QSet<QString> *skippedFonts)
{
    if (skippedFonts)
        skippedFonts->clear();
    auto fail = [error](const QString &message) {
        *error = message;
        return false;
    };
    if (bytes.size() < 22 || bytes.size() > maxBook)
        return fail(strings::get("error.unsupported_publication_size"));
    // Inspect the central directory before giving the archive to the engine.
    // Reject ZIP encryption and ZIP64 rather than guessing.
    Publication publication;
    if (!publication.inspect(bytes, error))
        return false;
    auto u16 = [&bytes](int p) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    auto u32 = [&bytes](int p) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar *>(bytes.constData() + p));
    };
    int end = -1;
    for (int p = bytes.size() - 22; p >= qMax(0, int(bytes.size()) - 65557); --p) {
        if (u32(p) == 0x06054b50 && p + 22 + u16(p + 20) == bytes.size()) {
            end = p;
            break;
        }
    }
    if (end < 0 || u16(end + 4) || u16(end + 6) || u16(end + 8) != u16(end + 10))
        return fail(strings::get("error.unsupported_epub_archive"));
    const quint32 offset = u32(end + 16), length = u32(end + 12);
    const int count = u16(end + 10);
    if (!count || count == 65535 || quint64(offset) + length != quint64(end))
        return fail(strings::get("error.unsupported_epub_directory"));
    int at = int(offset);
    quint64 expanded = 0;
    bool container = false;
    for (int i = 0; i < count; ++i) {
        if (at > end - 46 || u32(at) != 0x02014b50)
            return fail(strings::get("error.invalid_epub_directory"));
        const int nameSize = u16(at + 28), extra = u16(at + 30), comment = u16(at + 32);
        const quint64 next = quint64(at) + 46 + nameSize + extra + comment;
        if (next > quint64(end) || (u16(at + 8) & 0x41) || u32(at + 20) == 0xffffffffU)
            return fail(strings::get("error.encrypted_or_unsupported_epub"));
        const QByteArray name = bytes.mid(at + 46, nameSize);
        if (name == "META-INF/container.xml")
            container = true;
        expanded += u32(at + 24);
        if (expanded > 256 * 1024 * 1024)
            return fail(strings::get("error.expanded_publication_is_too_large"));
        at = int(next);
    }
    if (!container || at != end)
        return fail(strings::get("error.missing_epub_container"));
    if (skippedFonts)
        *skippedFonts = publication.skippedFonts();
    return true;
}

void setChapterModeOverride(int mode)
{
    chapterModeOverride.store(mode);
}

bool EngineDocument::open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error,
                          OpeningProgress progress)
{
    QSet<QString> skippedFonts;
    if (!supportedArchive(bytes, error, &skippedFonts))
        return false;
    auto source = std::make_shared<ReaderSource>();
    source->bytes = std::move(bytes);
    source->container = LVOpenArchieve(
            LVCreateMemoryStream(source->bytes.data(), source->bytes.size(), false, LVOM_READ));
    if (source->container.isNull()) {
        *error = strings::get("error.the_reading_engine_could_not_open_this_epub");
        return false;
    }
    return openSource(std::move(source), size, saved, error, std::move(progress), nullptr, {},
                      std::move(skippedFonts));
}

bool EngineDocument::openResources(std::shared_ptr<ResourceCache> resources,
                                   const QMap<QString, qint64> &sizes, QSize size,
                                   const QJsonObject &saved, QString *error,
                                   OpeningProgress progress, OpeningResult *result,
                                   std::function<void()> coverReady,
                                   std::function<void()> cacheReady)
{
    auto source = std::make_shared<ReaderSource>();
    source->resources = std::move(resources);
    source->sizes = sizes;
    source->container = LVContainerRef(new ResourceContainer(source->resources, sizes));
    return openSource(std::move(source), size, saved, error, std::move(progress), result,
                      std::move(coverReady), {}, std::move(cacheReady));
}

bool EngineDocument::rebindResources(const std::shared_ptr<ResourceCache> &resources,
                                     const QMap<QString, qint64> &sizes)
{
    // A spine item that was opened ahead holds the source too. It reads
    // through the old access, so it goes, with every other side document.
    dropSideDocuments();
    // Previews retain ReaderSource as their parent. Do not change their access
    // underneath them, even if the main reader itself has already closed.
    if (!source_ || source_.use_count() != 1 || !source_->resources || source_->sizes != sizes
        || !resources)
        return false;
    return source_->resources->rebind(*resources);
}

QString EngineDocument::resourceError() const
{
    return source_ && source_->resources ? source_->resources->error() : QString();
}

QJsonObject EngineDocument::resourceStats() const
{
    auto result = source_ && source_->resources ? source_->resources->statistics() : QJsonObject();
    for (auto at = openingTimes_.begin(); at != openingTimes_.end(); ++at)
        result.insert(at.key(), at.value());
    return result;
}

bool EngineDocument::openSource(std::shared_ptr<ReaderSource> source, QSize size,
                                const QJsonObject &saved, QString *error, OpeningProgress progress,
                                OpeningResult *result, std::function<void()> coverReady,
                                QSet<QString> skippedFonts, std::function<void()> cacheReady)
{
    DiagnosticOperation opening("engine-open", engineCancellationRequested);
    if (result)
        *result = {};
    const auto fail = [error, result](OpeningFailureKind kind, const char *reason,
                                      const QString &detail = QString()) {
        if (result)
            *result = OpeningResult::failed(kind, reason, detail);
        // Old direct-engine/preview callers still receive their original text.
        if (error)
            *error = detail.isEmpty() ? strings::get(reason) : detail;
        return false;
    };
    openingTimes_ = {};
    QElapsedTimer phase;
    phase.start();
    // Supplying a container skips format detection in the engine. Keep the
    // EPUB media-type check at this boundary for every resource provider.
    const QByteArray expectedType("application/epub+zip");
    auto mime = source->container->OpenStream(U"mimetype", LVOM_READ);
    QByteArray mediaType(expectedType.size(), '\0');
    lvsize_t read = 0;
    if (mime.isNull() || mime->GetSize() != lvsize_t(mediaType.size())
        || mime->Read(mediaType.data(), mediaType.size(), &read) != LVERR_OK
        || read != lvsize_t(mediaType.size()) || mediaType != expectedType) {
        if (source->resources && !source->resources->error().isEmpty())
            return fail(OpeningFailureKind::Io, "opening.resource-read",
                        source->resources->error());
        return fail(OpeningFailureKind::Unsupported,
                    strings::key("error.the_publication_is_not_an_epub"));
    }
    if (!initializeFonts())
        return fail(OpeningFailureKind::HostUnavailable,
                    strings::key("error.the_reading_font_is_unavailable"));
    configure(size, saved);
    openedFromCache_ = false;
    // Stream loading does not set a filename. CREngine uses its presence to
    // distinguish a loaded document from its internal blank/error document,
    // and suppresses pagination callbacks for the latter. With a fingerprint,
    // the name and checksum also select this book's cache file.
    auto properties = engine_->getDocProps();
    // Runtime only, never saved settings. The document cache holds the whole
    // book, so a window does not use it.
    int firstSpineItem = saved.value("koblaze_first_spine_item").toInt(-1);
    int lastSpineItem = saved.value("koblaze_last_spine_item").toInt(firstSpineItem);
#if __has_include(<koblaze.h>)
    // Experiment: KOBALT_CHAPTER_MODE=1 opens every book with the text of
    // the spine item of the saved position and its two neighbors only.
    if (chapterMode() && !saved.contains("koblaze_first_spine_item") && !source->parent) {
        const int item = qMax(0, spineItemIndex(saved.value("position").toString()));
        // KOBALT_CHAPTER_PAGES sets the pages of one layout. The default is
        // 25. Zero loads three spine items as a whole.
        const auto pages = qgetenv("KOBALT_CHAPTER_PAGES");
        slicePages_ = !pages.isEmpty()   ? qMax(0, pages.toInt())
                : chapterModePages() > 0 ? chapterModePages()
                                         : 25;
        firstSpineItem = slicePages_ > 0 ? item : qMax(0, item - 1);
        lastSpineItem = slicePages_ > 0 ? item : item + 1;
    }
#else
    firstSpineItem = lastSpineItem = -1;
#endif
    EngineSlowdownScope slowdown(firstSpineItem >= 0, [this] { return openInterrupt_ && openInterrupt_(); });
    QElapsedTimer windowTimer;
    windowTimer.start();
    windowFirst_ = firstSpineItem < 0 ? -1 : firstSpineItem;
    windowLast_ = firstSpineItem < 0 ? -1 : lastSpineItem;
    sliceFirst_ = sliceEnd_ = visiblePages_ = sliceForcedEnd_ = sliceExtra_ = 0;
    sliceHistory_.clear();
    const bool keyed =
            !cacheDirectory_.isEmpty() && !cacheFingerprint_.isEmpty() && firstSpineItem < 0;
    if (keyed) {
        properties->setString(
                DOC_PROP_FILE_NAME,
                Utf8ToUnicode(lString8(documentCacheName(cacheFingerprint_).toUtf8().constData())));
        properties->setHex(
                DOC_PROP_FILE_CRC32,
                documentCacheChecksum(documentCachePolicy() + ":bionic-v3:" + bionic_.toUtf8(),
                                      domVersion_));
        properties->setString(DOC_PROP_FILE_SIZE,
                              lString32::itoa(int(cacheBytes_ < 0                 ? 0
                                                          : cacheBytes_ > INT_MAX ? INT_MAX
                                                                                  : cacheBytes_)));
    } else
        properties->setString(DOC_PROP_FILE_NAME, lString32(U"publication.epub"));
    // Once for a source: showSpineItems() opens the same source again.
    if (!source->parent && !source->contentContainer) {
        source->container =
                LVContainerRef(new ContentContainer(source->container, std::move(skippedFonts)));
        source->contentContainer = true;
    }
    OpeningCallback callback(engine_, std::move(progress), openInterrupt_);
    source_ = std::move(source);
    openingTimes_["model_prepare_ms"] = double(phase.restart());
    bool loaded;
    {
        DocumentCacheScope cache(keyed && !bypassDocumentCache_ ? cacheDirectory_ : QString());
#if __has_include(<koblaze.h>)
#  ifdef KOBLAZE_SKIP_DOCUMENT_FONTS
        // With a reading font, no element uses a font of the book. The
        // engine then lists those fonts and does not read their files.
        fontsSkipped_ = firstSpineItem >= 0 && !face_.isEmpty();
        {
            auto fonts = LVCreatePropsContainer();
            fonts->setBool(PROP_EMBEDDED_FONTS, !fontsSkipped_);
            engine_->propsApply(fonts);
        }
#  endif
#  ifdef KOBLAZE_DOCUMENT_FONT_DATA
        // The fork keeps the data of embedded fonts for the next part of
        // the same book. Another book has other fonts under the same names.
        static quint64 fontSource = 0;
        const quint64 book = source_->parent ? source_->parent->identity : source_->identity;
        if (book != fontSource) {
            koblazeForgetDocumentFontData();
            fontSource = book;
        }
#  endif
        // Experimental, with the koblaze fork only: load the text of a
        // window of spine items. The others stay empty.
        koblazeSetSpineWindow(firstSpineItem, lastSpineItem);
        // The fork measures plain text from memories of glyph pairs and of
        // words. HarfBuzz fills them, and each face checks a sample of its
        // results against HarfBuzz and stops when one differs. The pages
        // are the same. KOBALT_FAST_SHAPING=0 turns this off, to compare.
        {
            const bool fast = qgetenv("KOBALT_FAST_SHAPING") != "0";
#  ifdef KOBLAZE_PAIR_KERNING
            koblazeSetPairKerning(fast);
#  endif
#  ifdef KOBLAZE_WORD_WIDTHS
            koblazeSetWordWidths(fast);
#  endif
#  ifdef KOBLAZE_WORD_PAIRS
            koblazeSetWordPairs(fast);
#  endif
#  ifdef KOBLAZE_PLAIN_JOINER
            koblazeSetPlainJoiner(fast);
#  endif
        }
#endif
#if __has_include(<koblaze.h>)
        // The loads of the other units of this book read the same package
        // and contents files.
        struct EpubCacheScope
        {
            explicit EpubCacheScope(KoblazeEpubCache *cache) { koblazeSetEpubCache(cache); }
            ~EpubCacheScope() { koblazeSetEpubCache(nullptr); }
        };
        const bool cached = packageCache_ && firstSpineItem >= 0 && !source_->parent;
        if (cached && !source_->epubCache)
            source_->epubCache = std::shared_ptr<KoblazeEpubCache>(koblazeCreateEpubCache(),
                                                                   koblazeDeleteEpubCache);
        const EpubCacheScope epubCache(cached ? source_->epubCache.get() : nullptr);
#endif
        loaded = engine_->LoadDocument(LVCreateMemoryStream(), false, source_->container);
#if __has_include(<koblaze.h>)
        koblazeSetSpineWindow(-1, -1);
#endif
        // A parsed document is rendered only after Render(); one restored
        // from the cache arrives rendered.
        openedFromCache_ = loaded && cache.enabled() && engine_->getDocument()->isRendered();
    }
    openingTimes_["model_cache_hit"] = openedFromCache_;
    if (openedFromCache_ && cacheReady)
        cacheReady();
    if (!loaded) {
        const auto resourceFailure = resourceError();
        engine_.reset();
        return resourceFailure.isEmpty()
                ? fail(OpeningFailureKind::InvalidContent,
                       strings::key("error.the_reading_engine_could_not_open_this_epub"))
                : fail(OpeningFailureKind::Io, "opening.resource-read", resourceFailure);
    }
    if (coverReady)
        coverReady();
    source_->embeddedFonts = source_->parent ? source_->parent->embeddedFonts : embeddedFontCount();
    applyPublisherFontFallbacks();
    auto *document = engine_->getDocument();
    const auto renderingPolicy = Utf8ToUnicode(lString8(KOBALT_RENDERING_POLICY));
    // A compatible engine update keeps the parsed DOM and Kobalt's markers.
    // Rebuild only its layout when shaping or hyphenation rules have changed.
    if (openedFromCache_
        && document->getProps()->getStringDef("kobalt.rendering.policy", "") != renderingPolicy) {
        document->forceReinitStyles();
        engine_->requestRender();
        document->setCacheFileStale(true);
    }
    document->getProps()->setString("kobalt.rendering.policy", renderingPolicy);
    document->getProps()->setInt("kobalt.bionic.mode", bionic_ == "bold" ? 1 : 0);
    openingTimes_["model_parse_ms"] = double(phase.restart());
    // The package language selects hyphenation patterns and line-breaking
    // rules for text without its own lang attribute.
    if (const auto language = engine_->getLanguage().trim(); !language.empty()) {
        auto properties = LVCreatePropsContainer();
        properties->setString(PROP_TEXTLANG_MAIN_LANG, language);
        engine_->propsApply(properties);
    }
#ifdef KOBLAZE_CANCEL_CHECK
    koblazeCheckCancel();
#endif
    // These optional markers can be added to older parsed caches without
    // changing source text, IDs or saved positions.
    markPageNotes();
    // A cached book was saved with these markers and its styles. Marking it
    // again would discard the cached pages.
    if (!openedFromCache_) {
        auto *document = engine_->getDocument();
        auto *root = document->getRootNode();
        if (root->getStyle().isNull()) {
            // Structural CSS selectors can invalidate styles after parsing.
            // Publisher alignment and leading-image margins need them before
            // Kobalt adds its layout markers, without paginating the document.
            auto *stylesheet = document->getStyleSheet();
            stylesheet->push();
            root->initNodeStyleRecursive(nullptr);
            stylesheet->pop();
        }
        markBodyAlignment(document);
        // Only the beautifier's style rules read its marks. A part of a
        // book loads again quickly when one of those settings is turned on.
        beautifierSkipped_ = firstSpineItem >= 0
                && !(beautifierInitials_ || beautifierSpacing_ || beautifierIndent_);
        if (!beautifierSkipped_)
            markBeautifierContent(document);
        markChapterLeadingImages(engine_.get());
        markKoboLinkDecorations();
        // Loading computed styles before the body markers existed, including in previews.
        engine_->getDocument()->forceReinitStyles();
    }
#ifdef KOBLAZE_CANCEL_CHECK
    koblazeCheckCancel();
#endif
#ifdef KOBLAZE_DROP_EMPTY_PAGES
    // Two forced page breaks with nothing between them, at the start of
    // some chapters, give a page without height: the reader saw an empty
    // page before the chapter. Leave such pages out.
    engine_->getDocument()->setKoblazeDropEmptyPages(true);
#endif
    applyStyleSheet();
    applySlice();
    engine_->Render();
    openingTimes_["model_paginate_ms"] = double(phase.restart());
    cutSlice();
#if __has_include(<koblaze.h>)
    // The first slice also ends at a page that starts a block.
    if (!extendSlice())
        throw KoblazeRenderCancelled();
#endif
    spineItemCount_ = spineItems().size();
    windowFirst_ = firstSpineItem < 0 ? -1 : firstSpineItem;
    windowLast_ = firstSpineItem < 0 ? -1 : qMin(lastSpineItem, spineItemCount_ - 1);
    // A book that opens for the first time continues its saved page estimate.
    // Later windows of the same book keep the one they have.
    if (windowFirst_ >= 0 && source_->pageEstimate.map.items() != spineItemCount_)
        restorePageEstimate(saved.value("page_estimate"));
    engineSlowdownPause();
    windowLoadMs_ = firstSpineItem < 0 ? -1 : windowTimer.elapsed();
    if (!resourceError().isEmpty()) {
        const auto detail = resourceError();
        engine_.reset();
        return fail(OpeningFailureKind::Io, "opening.resource-read", detail);
    }
    // A window without content still has one page, without height.
    bool noPages = !pages();
    if (!noPages && firstSpineItem >= 0) {
        const auto *list = engine_->getPageList();
        noPages = true;
        for (int i = 0; list && i < list->length() && noPages; ++i)
            noPages = list->get(i)->height <= 0;
    }
    if (noPages && firstSpineItem >= 0 && spineItemCount_ > 0
        && (firstSpineItem > 0 || windowLast_ + 1 < spineItemCount_)) {
        // A spine item can have no page: an empty file, or content that is
        // not shown. As a part of the whole book it adds nothing. Loaded
        // alone it must not end the book: load it with the spine item
        // beside it, on the side that the reader is going to.
        auto wider = saved;
        const bool forward = (emptyUnitDirection_ >= 0 && windowLast_ + 1 < spineItemCount_)
                || firstSpineItem == 0;
        wider["koblaze_first_spine_item"] = forward ? firstSpineItem : firstSpineItem - 1;
        wider["koblaze_last_spine_item"] = forward ? windowLast_ + 1 : windowLast_;
        return openSource(source_, size, wider, error, {}, result);
    }
    if (!pages()) {
        engine_.reset();
        return fail(OpeningFailureKind::InvalidContent,
                    strings::key("error.the_publication_has_no_pages"));
    }
    // The first layout gives its pages too, also when the saved position
    // is in another slice. Each later layout observes itself.
    observeLayout();
    engine_->getCurPage();
    anchor_ = qstring(engine_->getBookmark().toString());
    const auto bookmark = saved.value("position").toString();
    if (!bookmark.isEmpty() && !goTo(bookmark)) {
        engine_.reset();
        return fail(OpeningFailureKind::InvalidContent,
                    strings::key("error.saved_reading_position_no_longer_exists"));
    }
    // The first slice has a layout now. The saved position can be in
    // another one.
    if (slices() && !saved.value("position").toString().isEmpty())
        goTo(saved.value("position").toString());
    return true;
}

bool EngineDocument::showSpineItemOf(const QString &position)
{
    if (windowFirst_ < 0)
        return true;
    const int item = spineItemIndex(position);
    if (item < 0 || (item >= windowFirst_ && item <= windowLast_))
        return true;
    if (slicePages_ > 0) {
        const auto unit = unitFrom(item);
        return showSpineItems(unit.first, unit.second);
    }
    return showSpineItems(qMax(0, item - 1), qMin(spineItemCount_ - 1, item + 1));
}

QJsonObject EngineDocument::windowSettings(int first, int last) const
{
    // Without the page estimate: it changes with every page, and the
    // settings decide whether the document prepared ahead can be shown.
    auto saved = settingsState();
    saved["koblaze_first_spine_item"] = first;
    saved["koblaze_last_spine_item"] = last;
    if (first >= 0) {
        // Outside the window a position has no text, so the book opens at
        // the start of the window.
        const int item = spineItemIndex(saved.value("position").toString());
        if (item < first || item > last)
            saved.remove("position");
    }
    return saved;
}

bool EngineDocument::setChapterMode(bool fast, const appearance::Patch &patch, QSize size,
                                    OpeningProgress progress, const std::function<bool()> &current)
{
#if __has_include(<koblaze.h>)
    if (!source_ || !engine_ || (current && !current()))
        return false;
    auto saved = state();
    for (auto it = patch.fields().begin(); it != patch.fields().end(); ++it)
        saved[it.key()] = it.value();
    saved.remove("page_estimate");
    const int item = fast ? qMax(0, spineItemIndex(position())) : -1;
    saved["koblaze_first_spine_item"] = item;
    saved["koblaze_last_spine_item"] = item;
    EngineDocument next(fonts_);
    next.slicePages_ = fast ? qMax(1,
                                   qgetenv("KOBALT_CHAPTER_PAGES").isEmpty()
                                           ? 25
                                           : qgetenv("KOBALT_CHAPTER_PAGES").toInt())
                            : 0;
    next.userStyleSheet_ = userStyleSheet_;
    next.cacheDirectory_ = cacheDirectory_;
    next.bypassDocumentCache_ = true;
    next.cacheFingerprint_ = cacheFingerprint_;
    next.cachePolicy_ = cachePolicy_;
    next.cacheBytes_ = cacheBytes_;
    next.interrupt_ = interrupt_;
    next.openInterrupt_ = [current] { return current && !current(); };
    next.nativeLocation_ = nativeLocation_;
    QString error;
    // Keep the displayed document usable until the replacement is complete.
#  ifdef KOBLAZE_CANCEL_CHECK
    const StopCheck check(current);
    try {
#  endif
        const auto viewport = size.isValid() ? size : size_;
        const bool opened = source_->resources
                ? next.openResources(source_->resources->fresh(), source_->sizes, viewport, saved,
                                     &error, std::move(progress))
                : next.open(source_->bytes, viewport, saved, &error, std::move(progress));
        if (!opened || (current && !current()))
            return false;
#  ifdef KOBLAZE_CANCEL_CHECK
    } catch (const KoblazeRenderCancelled &) {
        koblazeSetSpineWindow(-1, -1);
        return false;
    }
#  endif
    next.openInterrupt_ = nullptr;
    next.bypassDocumentCache_ = false;
    if (fast) {
        next.setChapters(contents_, graph_);
        next.externalChapters_ = externalChapters_;
        next.totalWords_ = totalWords_;
    } else {
        next.wordCounts(current);
        if (externalChapters_) {
            next.setChapters(contents_, graph_);
            next.externalChapters_ = true;
        }
    }
    if (current && !current())
        return false;
    EngineDocument old(fonts_);
    old = std::move(*this);
    *this = std::move(next);
    return true;
#else
    Q_UNUSED(fast);
    Q_UNUSED(current);
    return applyAppearance(patch, size, std::move(progress));
#endif
}

bool EngineDocument::showSpineItems(int first, int last, QString *error)
{
    DiagnosticOperation turning("chapter-turn", engineCancellationRequested);
    EngineSlowdownScope slowdown;
    if (!source_ || !engine_)
        return false;
    if (adoptAhead(first, last)) {
        if (diagnosticsActive())
            diagnostic("engine-cache-reuse", { { "first_item", first }, { "last_item", last } }, false);
        // An explicit chapter jump starts at its first slice, even when
        // the retained document was left at the end of the chapter.
        return !slices() || sliceFirst_ == 0 || showSliceAt(0);
    }
    if (slicePages_ > 0 && windowFirst_ >= 0) {
        if (!openAhead(first, {}, last, error))
            return false;
        const qint64 loadMs = ahead_->windowLoadMs_;
        if (!adoptAhead(first, last))
            return false;
        windowLoadMs_ = loadMs;
        // A document that was kept in memory can be in a later slice.
        return !slices() || sliceFirst_ == 0 || showSliceAt(0);
    }
    ahead_.reset();
    const auto contents = contents_, graph = graph_;
    const auto externalChapters = externalChapters_;
    const auto words = totalWords_;
    const bool opened = openSource(source_, size_, windowSettings(first, last), error);
    if (opened) {
        setChapters(contents, graph);
        externalChapters_ = externalChapters;
        totalWords_ = words;
    }
    return opened;
}

QPair<int, int> EngineDocument::neighbouringUnit(int direction) const
{
    const int item = direction > 0 ? windowLast_ + 1 : windowFirst_ - 1;
    // Forward and backward grouping can choose different ranges of short
    // items. Keep the actual adjacent range when it is already available.
    if (ahead_ && (direction > 0 ? ahead_->windowFirst_ == item : ahead_->windowLast_ == item)) {
        auto settings = windowSettings(ahead_->windowFirst_, ahead_->windowLast_);
        settings.remove("position");
        if (settings == aheadSettings_ && size_ == aheadSize_
            && userStyleSheet_ == ahead_->userStyleSheet_ && slicePages_ == ahead_->slicePages_)
            return { ahead_->windowFirst_, ahead_->windowLast_ };
    }
    return direction > 0 ? unitFrom(item) : unitUntil(item);
}

void EngineDocument::prepareNextItem(const std::function<bool()> &current)
{
    if (slicePages_ <= 0 || windowFirst_ < 0)
        return;
    const int next = windowLast_ + 1;
    // In a spine item with slices, only in the last slice: a second
    // document takes memory, and the reader is far from the next unit
    // before that.
    if (next >= spineItemCount_ || (slices() && sliceEnd_ < spineItemText()))
        return;
    const auto unit = neighbouringUnit(1);
    auto *prepared = openAhead(unit.first, current, unit.second);
#if defined(KOBLAZE_CANCEL_CHECK)
    // A kept unit can have been left in a later slice. The turn must not
    // wait for the layout of its first slice.
    if (!prepared || !prepared->slices() || prepared->sliceFirst_ == 0)
        return;
    const StopCheck check(current);
    prepared->sliceInterrupt_ = [&current] { return current && !current(); };
    try {
        if (!prepared->showSliceAt(0) || (current && !current()))
            ahead_.reset();
        else
            prepared->sliceInterrupt_ = nullptr;
    } catch (const KoblazeRenderCancelled &) {
        koblazeSetSpineWindow(-1, -1);
        ahead_.reset();
    }
#else
    Q_UNUSED(prepared);
#endif
}

void EngineDocument::preparePreviousItem(const std::function<bool()> &current)
{
#if defined(KOBLAZE_CANCEL_CHECK)
    if (slicePages_ <= 0 || windowFirst_ <= 0 || page() != 0 || (slices() && sliceFirst_ > 0)
        || (current && !current()))
        return;
    const auto unit = neighbouringUnit(-1);
    const int direction = emptyUnitDirection_;
    emptyUnitDirection_ = -1;
    auto *previous = openAhead(unit.first, current, unit.second);
    emptyUnitDirection_ = direction;
    if (!previous)
        return;
    const StopCheck check(current);
    previous->sliceInterrupt_ = [&current] { return current && !current(); };
    try {
        if (previous->slices() && previous->sliceEnd_ < previous->spineItemText()
            && !previous->showSliceAt(qMax(0, previous->spineItemText() - 1))) {
            ahead_.reset();
            return;
        }
        if (current && !current()) {
            ahead_.reset();
            return;
        }
        previous->go(qMax(0, previous->pages() - 1));
        previous->image(previous->page());
        previous->sliceInterrupt_ = nullptr;
    } catch (const KoblazeRenderCancelled &) {
        koblazeSetSpineWindow(-1, -1);
        ahead_.reset();
    }
#else
    Q_UNUSED(current);
#endif
}

EngineDocument *EngineDocument::openAhead(int item, const std::function<bool()> &current, int last,
                                          QString *error)
{
#if __has_include(<koblaze.h>)
    if (slicePages_ <= 0 || windowFirst_ < 0 || !source_ || !engine_ || item < 0
        || item >= spineItemCount_)
        return nullptr;
    const auto unit = last >= item ? qMakePair(item, last) : unitFrom(item);
    auto settings = windowSettings(unit.first, unit.second);
    settings.remove("position");
    if (ahead_ && aheadItem_ == item && aheadSettings_ == settings && aheadSize_ == size_
        && ahead_->userStyleSheet_ == userStyleSheet_ && ahead_->slicePages_ == slicePages_)
        return ahead_.get();
    // The document that was here stays in memory while there is room.
    keepUnit(std::move(ahead_), aheadSettings_, aheadSize_);
    auto ahead = takeKept(unit.first, settings);
    if (!ahead) {
        DiagnosticOperation load("chapter-load", [&current] {
            return (current && !current()) || engineCancellationRequested();
        });
        ahead = openSide(settings, current, error);
        if (!ahead && !kept_.empty() && (!current || current())) {
            // The kept documents must never cost the reader a chapter:
            // without them the engine has room for this one.
            kept_.clear();
            if (error)
                error->clear();
            ahead = openSide(settings, current, error);
        }
    }
    if (!ahead)
        return nullptr;
    ahead_ = std::move(ahead);
    aheadItem_ = item;
    aheadSettings_ = settings;
    aheadSize_ = size_;
    return ahead_.get();
#else
    Q_UNUSED(item);
    Q_UNUSED(current);
    Q_UNUSED(last);
    Q_UNUSED(error);
    return nullptr;
#endif
}

std::unique_ptr<EngineDocument> EngineDocument::openSide(const QJsonObject &settings,
                                                         const std::function<bool()> &current,
                                                         QString *error, bool packageCache)
{
#if __has_include(<koblaze.h>)
    auto side = std::make_unique<EngineDocument>(fonts_);
    side->packageCache_ = packageCache;
    // What the host gave this document and a saved state does not hold.
    side->slicePages_ = slicePages_;
    side->userStyleSheet_ = userStyleSheet_;
    side->cacheDirectory_ = cacheDirectory_;
    side->cacheFingerprint_ = cacheFingerprint_;
    side->cachePolicy_ = cachePolicy_;
    side->cacheBytes_ = cacheBytes_;
    side->emptyUnitDirection_ = emptyUnitDirection_;
#  ifdef KOBLAZE_CANCEL_CHECK
    // A tap must not wait for this work. The load and the style pass ask
    // the check, the layout asks the interrupt. A stopped document is
    // dropped, and the next call starts again.
    const StopCheck check(current);
    side->openInterrupt_ = [&check] { return check.requested(); };
    try {
        if (!side->openSource(source_, size_, settings, error))
            return nullptr;
    } catch (const KoblazeRenderCancelled &) {
        koblazeSetSpineWindow(-1, -1);
        return nullptr;
    }
    side->openInterrupt_ = nullptr;
#  else
    Q_UNUSED(current);
    if (!side->openSource(source_, size_, settings, error))
        return nullptr;
#  endif
    return side;
#else
    Q_UNUSED(packageCache);
    Q_UNUSED(settings);
    Q_UNUSED(current);
    Q_UNUSED(error);
    return nullptr;
#endif
}

void EngineDocument::setBackgroundCount(bool enabled)
{
    if (source_)
        source_->pageEstimate.count.store(enabled);
}

bool EngineDocument::backgroundCount() const
{
    return source_ && source_->pageEstimate.count.load();
}

void EngineDocument::keepUnit(std::unique_ptr<EngineDocument> document, const QJsonObject &settings,
                              QSize size)
{
    // About six times this in memory on a 64-bit desktop.
    static constexpr qint64 keptUnitBytes = 6 * 1024 * 1024;
    // The engine has room for 16 documents at a time
    // (MAX_DOCUMENT_INSTANCE_COUNT). The document on screen, the one
    // opened ahead, the one of the count, a preview, the three of the
    // package cache and the short-lived documents of a load need theirs.
    static constexpr int keptUnitCount = 4;
    if (!document || !source_ || document->windowFirst_ < 0)
        return;
    const int first = document->windowFirst_, last = document->windowLast_;
    // Only a document for the page of this one can be shown later.
    auto current = windowSettings(first, last);
    current.remove("position");
    if (settings != current || size != size_ || document->userStyleSheet_ != userStyleSheet_
        || document->slicePages_ != slicePages_)
        return;
    const auto &bytes = spineBytes();
    KeptUnit unit;
    unit.first = first;
    unit.settings = settings;
    unit.size = size;
    for (int item = first; item <= last && item < bytes.size(); ++item)
        unit.bytes += bytes[item];
    unit.document = std::move(document);
    for (auto it = kept_.begin(); it != kept_.end();) {
        if (it->first == first)
            it = kept_.erase(it);
        else
            ++it;
    }
    kept_.push_back(std::move(unit));
    qint64 total = 0;
    for (const auto &entry : kept_)
        total += entry.bytes;
    while ((total > keptUnitBytes || int(kept_.size()) > keptUnitCount) && !kept_.empty()) {
        auto farthest = kept_.begin();
        for (auto it = kept_.begin(); it != kept_.end(); ++it) {
            if (qAbs(it->first - windowFirst_) > qAbs(farthest->first - windowFirst_))
                farthest = it;
        }
        total -= farthest->bytes;
        kept_.erase(farthest);
    }
}

std::unique_ptr<EngineDocument> EngineDocument::takeKept(int first, const QJsonObject &settings)
{
    for (auto it = kept_.begin(); it != kept_.end(); ++it) {
        if (it->first != first)
            continue;
        auto document = std::move(it->document);
        const bool usable = it->settings == settings && it->size == size_
                && document->userStyleSheet_ == userStyleSheet_
                && document->slicePages_ == slicePages_;
        kept_.erase(it);
        return usable ? std::move(document) : nullptr;
    }
    return nullptr;
}

void EngineDocument::dropSideDocuments()
{
    if (source_ && !source_->parent)
        source_->epubCache.reset();
    ahead_.reset();
    counting_.reset();
    countingNext_ = -1;
    kept_.clear();
}

void EngineDocument::reportCountEnd()
{
    if (countReported_ || !diagnosticsActive())
        return;
    countReported_ = true;
    const auto &map = pageMap();
    diagnostic("page-count-end",
               { { "spine_items", map.items() },
                 { "counted", int(map.completeItems().size()) },
                 { "not_counted", int(uncounted_.size()) },
                 { "exact", map.counted() },
                 { "pages", qRound(map.total()) } },
               false);
}

void EngineDocument::adoptCounted()
{
    if (windowFirst_ < 0 || !source_ || !engine_)
        return;
    // Once the spine items up to the page are counted, the page number is
    // the real one. The total stays an estimate until the count ends.
    const auto place = roughPlace();
    if (pageMap().adoptCounted(place.item))
        source_->pageEstimate.lastItem = -1;
}

bool EngineDocument::countPages(const std::function<bool()> &current)
{
#if __has_include(<koblaze.h>) && defined(KOBLAZE_CANCEL_CHECK)
    if (slicePages_ <= 0 || windowFirst_ < 0 || !source_ || source_->parent || !engine_)
        return false;
    auto &map = pageMap();
    // The estimate comes first: it gives the numbers that are shown until
    // the count ends.
    if (!map.complete() || map.counted())
        return false;
    const auto key = estimateKey(settingsState());
    if (key != countingKey_ || size_ != countingSize_) {
        // Another page: the counts so far were for the old one.
        countReported_ = false;
        source_->epubCache.reset();
        counting_.reset();
        countingNext_ = -1;
        kept_.clear();
        uncounted_.clear();
        countingKey_ = key;
        countingSize_ = size_;
    }
    if (!counting_) {
        // The spine items before the page come first, the nearest first:
        // with them the page number is the real one. Then the spine item of
        // the page, then the rest of the book.
        const auto counted = map.completeItems();
        const auto open = [&](int item) {
            return !counted.contains(item) && !uncounted_.contains(item);
        };
        int item = windowFirst_ - 1;
        while (item >= 0 && !open(item))
            --item;
        if (item < 0) {
            item = windowFirst_;
            while (item < spineItemCount_ && !open(item))
                ++item;
        }
        if (item >= spineItemCount_) {
            // Nothing left to try. Spine items that gave no count stay
            // estimates.
            source_->epubCache.reset();
            adoptCounted();
            reportCountEnd();
            return false;
        }
        // A unit can start before the item: count from its start.
        auto unit = unitFrom(item);
        for (int start = item - 1; start >= 0 && unitFrom(start).second >= item; --start)
            unit = unitFrom(start);
        auto settings = windowSettings(unit.first, unit.second);
        settings.remove("position");
        // The layout at the open observes the first slice, or each spine
        // item of a unit as a whole.
        counting_ = openSide(settings, current, nullptr, true);
        countingItem_ = item;
        countingNext_ = -1;
        countingSettings_ = settings;
        if (!counting_ && (!current || current())) {
            // Without the kept documents the engine has room for this one.
            if (!kept_.empty())
                kept_.clear();
            else
                uncounted_.insert(item);
        }
        return true;
    }
    auto *side = counting_.get();
    if (countingNext_ >= 0 || (side->slices() && side->sliceEnd_ < side->spineItemText())) {
        const int next = countingNext_ >= 0 ? countingNext_ : side->sliceEnd_;
        side->sliceInterrupt_ = [&current] { return current && !current(); };
        bool laidOut = false;
        try {
            laidOut = side->layoutSlice(next);
        } catch (const KoblazeRenderCancelled &) {
            koblazeSetSpineWindow(-1, -1);
            laidOut = false;
        }
        side->sliceInterrupt_ = nullptr;
        if (laidOut) {
            countingNext_ = -1;
            return true;
        }
        if (side->layoutInterrupted() || (current && !current())) {
            // A stopped layout is not a count. Only this slice is laid out
            // again: the slices before it keep theirs.
            countingNext_ = next;
            return true;
        }
        uncounted_.insert(countingItem_);
        counting_.reset();
        countingNext_ = -1;
        return true;
    }
    // The counted document stays in memory while there is room.
    keepUnit(std::move(counting_), countingSettings_, countingSize_);
    // A spine item that did not get its pages is not tried again.
    if (!map.completeItems().contains(countingItem_))
        uncounted_.insert(countingItem_);
    adoptCounted();
    if (map.counted()) {
        source_->epubCache.reset();
        reportCountEnd();
    }
    return !map.counted();
#else
    Q_UNUSED(current);
    return false;
#endif
}

bool EngineDocument::adoptAhead(int first, int last)
{
    if (!ahead_ || aheadItem_ != first)
        return false;
    // The settings hold the unit: another range does not match.
    auto settings = windowSettings(first, last);
    settings.remove("position");
    if (settings != aheadSettings_ || size_ != aheadSize_
        || ahead_->userStyleSheet_ != userStyleSheet_ || ahead_->slicePages_ != slicePages_)
        return false;
    auto next = std::move(ahead_);
    next->setChapters(contents_, graph_);
    next->externalChapters_ = externalChapters_;
    next->totalWords_ = totalWords_;
    next->interrupt_ = interrupt_;
    next->nativeLocation_ = nativeLocation_;
    next->emptyUnitDirection_ = emptyUnitDirection_;
    // The reader did not wait for this spine item.
    next->windowLoadMs_ = 0;
    // Move the departed unit into the same spare slot. Neither document
    // owns another neighbour, so repeated crossings cannot grow a chain.
    const int oldItem = windowFirst_;
    auto oldSettings = windowSettings(windowFirst_, windowLast_);
    oldSettings.remove("position");
    const auto oldSize = size_;
    auto old = std::make_unique<EngineDocument>(fonts_);
    *old = std::move(*this);
    *this = std::move(*next);
    // The count goes on in the document that is shown.
    counting_ = std::move(old->counting_);
    countingKey_ = std::move(old->countingKey_);
    countingSize_ = old->countingSize_;
    countingItem_ = old->countingItem_;
    countingNext_ = old->countingNext_;
    countReported_ = old->countReported_;
    countingSettings_ = std::move(old->countingSettings_);
    uncounted_ = std::move(old->uncounted_);
    kept_ = std::move(old->kept_);
    ahead_ = std::move(old);
    aheadItem_ = oldItem;
    aheadSettings_ = std::move(oldSettings);
    aheadSize_ = oldSize;
    return true;
}

bool EngineDocument::showNextUnit()
{
    DiagnosticOperation turning("chapter-turn", engineCancellationRequested);
    EngineSlowdownScope slowdown;
    const int last = windowLast_;
    if (last + 1 >= spineItemCount_)
        return false;
    const auto unit = neighbouringUnit(1);
    if (!showSpineItems(unit.first, unit.second))
        return false;
    // The rest of the book has no pages. The window then holds the spine
    // item that was shown too: stay on its last page.
    if (windowFirst_ <= last) {
        go(pages() - 1);
        return false;
    }
    // A retained unit can have been left in a later slice.
    return !slices() || sliceFirst_ == 0 || showSliceAt(0);
}

bool EngineDocument::showPreviousUnit()
{
    DiagnosticOperation turning("chapter-turn", engineCancellationRequested);
    EngineSlowdownScope slowdown;
    if (windowFirst_ <= 0)
        return false;
    const auto unit = neighbouringUnit(-1);
    emptyUnitDirection_ = -1;
    // A backward page turn can use the retained last slice directly.
    const bool shown = adoptAhead(unit.first, unit.second)
            || showSpineItems(unit.first, unit.second);
    emptyUnitDirection_ = 1;
    if (!shown)
        return false;
    // A spine item with slices opens with its first slice. Its last page
    // is in its last slice.
    if (slices()) {
        const int text = spineItemText();
        if (sliceEnd_ < text && !showSliceAt(qMax(0, text - 1)))
            return false;
    }
    return true;
}

bool EngineDocument::turnInWindow(int delta)
{
    if (!engine_ || !delta)
        return false;
    // On the extra pages after the end of a slice, the next slice is due.
    // prepareAhead() usually did this already.
    if (delta > 0 && pastSliceEnd() && !enterNextSlice())
        return false;
    // The last page that has a layout stops where the layout stops, so it
    // is not a whole page while the slice continues. With one extra page
    // only, the next page is that one: lay out the next slice first.
    if (delta > 0 && slices() && visiblePages_ > 0 && sliceEnd_ < spineItemText()
        && page() + 1 == visiblePages_ && visiblePages_ + 1 >= pages()) {
        sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
        return layoutSlice(sliceEnd_) && pages() > 0 && go(0);
    }
    const int target = turnTarget(page(), delta);
    if (target != page())
        return go(target);
    if (windowFirst_ < 0)
        return false;
    if (slices()) {
        // The next or the previous slice of this spine item, then the
        // spine item beside it.
        // A slice has no pages when no block starts in it: one block, such
        // as a table, can be longer than a slice. Pass such slices.
        if (delta > 0) {
            for (;;) {
                if (slices() && sliceEnd_ < spineItemText() && visiblePages_ == 0) {
                    sliceHistory_.append({ sliceFirst_, sliceForcedEnd_ });
                    if (!layoutSlice(sliceEnd_))
                        return false;
                } else if (!showNextUnit()) {
                    return false;
                }
                if (pages() > 0)
                    return go(0);
            }
        }
        for (;;) {
            if (slices() && sliceFirst_ > 0) {
                // A recorded start gives the same pages as before. Without
                // one, the slice before must stop where this one starts.
                const auto slice = sliceHistory_.isEmpty()
                        ? qMakePair(qMax(0, sliceFirst_ - sliceSize()), sliceFirst_)
                        : sliceHistory_.takeLast();
                if (!layoutSlice(slice.first, slice.second))
                    return false;
            } else if (!showPreviousUnit()) {
                return false;
            }
            // The extra pages after the end of the slice repeat the start
            // of the slice that the reader came from.
            if (pages() > 0)
                return go((visiblePages_ > 0 ? visiblePages_ : pages()) - 1);
        }
    }
    if (slicePages_ > 0) {
        // A unit of several short spine items. Pass a unit without pages.
        for (;;) {
            if (!(delta > 0 ? showNextUnit() : showPreviousUnit()))
                return false;
            if (pages() > 0)
                return go(delta > 0 ? 0 : (visiblePages_ > 0 ? visiblePages_ : pages()) - 1);
        }
    }
    const auto items = spineItems();
    const int count = items.size();
    const int item = delta > 0 ? windowLast_ + 1 : windowFirst_ - 1;
    if (item < 0 || item >= count)
        return false;
    if (!showSpineItems(qMax(0, item - 1), qMin(count - 1, item + 1)))
        return false;
    Location start;
    if (delta > 0) {
        start.href = items[item];
        return goTo(positionOf(start));
    }
    // The last page of the spine item before the one that was first.
    start.href = items[item + 1];
    const int next = pagesOf({ positionOf(start) }).value(0, -1);
    return next > 0 && go(next - 1);
}

PagePreview EngineDocument::previewTurn(int direction,
                                        const QVector<QPair<QString, QString>> &ranges,
                                        const std::function<bool()> &current,
                                        const QStringList &bookmarkPositions)
{
    if (!engine_ || direction < -1 || direction > 1 || (current && !current()))
        return {};
    const int target = direction ? turnTarget(page(), direction) : page();
    const bool tail = direction > 0 && slices() && visiblePages_ > 0 && target >= visiblePages_;
    if ((!direction || target != page()) && !tail) {
        PagePreview result;
        result.image = image(target);
        result.position = pageStart(target);
        result.ranges = pageRangeRects(target, ranges);
        result.bookmarked =
                !bookmarkPositions.isEmpty() && pagesOf(bookmarkPositions).contains(target);
        return current && !current() ? PagePreview{} : result;
    }
    if (windowFirst_ < 0 || !source_)
        return {};
    auto preview = std::make_unique<EngineDocument>(fonts_);
    preview->slicePages_ = slicePages_;
    preview->userStyleSheet_ = userStyleSheet_;
    preview->cacheDirectory_ = cacheDirectory_;
    preview->cacheFingerprint_ = cacheFingerprint_;
    preview->cachePolicy_ = cachePolicy_;
    preview->cacheBytes_ = cacheBytes_;
    auto settings = windowSettings(windowFirst_, windowLast_);
    settings["position"] = position();
    QString error;
#ifdef KOBLAZE_CANCEL_CHECK
    const StopCheck check(current);
    preview->openInterrupt_ = [&current] { return current && !current(); };
    preview->sliceInterrupt_ = preview->openInterrupt_;
    try {
#endif
        if (!preview->openSource(source_, size_, settings, &error) || (current && !current())
            || !preview->turnInWindow(direction))
            return {};
        PagePreview result;
        result.image = preview->image(preview->page());
        result.position = preview->pageStart(preview->page());
        result.ranges = preview->pageRangeRects(preview->page(), ranges);
        result.bookmarked = !bookmarkPositions.isEmpty()
                && preview->pagesOf(bookmarkPositions).contains(preview->page());
        return current && !current() ? PagePreview{} : result;
#ifdef KOBLAZE_CANCEL_CHECK
    } catch (const KoblazeRenderCancelled &) {
        koblazeSetSpineWindow(-1, -1);
        return {};
    }
#endif
}

bool EngineDocument::openPreview(Engine &other, QSize size, QString *error)
{
    auto fail = [error](const QString &message) {
        *error = message;
        return false;
    };
    // A preview shares the parsed book of its source, so both are CREngine.
    if (qstrcmp(other.name(), name()) != 0)
        return fail(strings::get("error.no_section_to_preview"));
    auto &source = static_cast<EngineDocument &>(other);
    if (!source.engine_)
        return fail(strings::get("error.no_section_to_preview"));
    auto *document = source.engine_->getDocument();
    auto pointer = document->createXPointer(cre(source.position()));
    auto *fragment = pointer.isNull() ? nullptr : pointer.getNode();
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    if (!fragment)
        return fail(strings::get("error.could_not_locate_the_current_section"));
    const auto section = qstring(fragment->getAttributeValue("Source"));
    // The document container already decodes obfuscated fonts. Loading it as
    // another EPUB would apply that transformation twice. Previews must start
    // from the original resource container, like the main document.
    auto container = source.source_ ? source.source_->container : LVContainerRef();
    if (container.isNull())
        return fail(strings::get("error.the_section_resources_are_unavailable"));
    bool resourceError = false;
    auto read = [&](const QString &name) -> QByteArray {
        auto stream = container->OpenStream(cre(name).c_str(), LVOM_READ);
        if (stream.isNull() || stream->GetSize() > Publication::maxResource) {
            resourceError = true;
            return {};
        }
        QByteArray data(int(stream->GetSize()), '\0');
        lvsize_t count = 0;
        if (stream->Read(data.data(), data.size(), &count) != LVERR_OK
            || count != lvsize_t(data.size())) {
            resourceError = true;
            return {};
        }
        return data;
    };
    QXmlStreamReader descriptor(read("META-INF/container.xml"));
    QString packagePath;
    while (!descriptor.atEnd()) {
        descriptor.readNext();
        if (descriptor.isStartElement() && descriptor.name() == QStringLiteral("rootfile")) {
            packagePath = descriptor.attributes().value("full-path").toString();
            break;
        }
    }
    if (packagePath.isEmpty())
        return fail(strings::get("error.the_publication_package_is_unavailable"));
    const auto package = read(packagePath);
    QXmlStreamReader manifest(package);
    QString sectionId;
    while (!manifest.atEnd()) {
        manifest.readNext();
        if (manifest.isStartElement() && manifest.name() == QStringLiteral("item")) {
            const auto href =
                    QUrl::fromPercentEncoding(manifest.attributes().value("href").toUtf8());
            if (QDir::cleanPath(QFileInfo(packagePath).path() + '/' + href) == section)
                sectionId = manifest.attributes().value("id").toString();
        }
    }
    if (manifest.hasError() || sectionId.isEmpty())
        return fail(strings::get("error.the_current_section_is_not_in_the_manifest"));
    QByteArray previewPackage;
    QXmlStreamReader input(package);
    QXmlStreamWriter output(&previewPackage);
    bool selected = false, inSpine = false;
    while (!input.atEnd()) {
        input.readNext();
        if (input.isStartElement() && input.name() == QStringLiteral("spine"))
            inSpine = true;
        if (inSpine && input.isStartElement() && input.name() == QStringLiteral("itemref")) {
            if (selected || input.attributes().value("idref") != sectionId) {
                input.skipCurrentElement();
                continue;
            }
            selected = true;
        }
        const bool spine = input.isStartElement() && input.name() == QStringLiteral("spine");
        const bool navigation = input.isStartElement() && input.name() == QStringLiteral("item")
                && input.attributes()
                           .value("properties")
                           .toString()
                           .split(QRegularExpression("\\s+"), qt::skipEmptyParts)
                           .contains("nav");
        if (spine || navigation) {
            // This package has one section. Whole-book navigation otherwise
            // points at absent fragments and adds warnings to every preview.
            output.writeStartElement(input.qualifiedName().toString());
            for (const auto &declaration : input.namespaceDeclarations())
                output.writeNamespace(declaration.namespaceUri().toString(),
                                      declaration.prefix().toString());
            for (const auto &attribute : input.attributes()) {
                if (spine
                    && (attribute.name() == QStringLiteral("toc")
                        || attribute.name() == QStringLiteral("page-map")))
                    continue;
                auto value = attribute.value().toString();
                if (navigation && attribute.name() == QStringLiteral("properties")) {
                    auto properties = value.split(QRegularExpression("\\s+"), qt::skipEmptyParts);
                    properties.removeAll("nav");
                    value = properties.join(' ');
                }
                output.writeAttribute(attribute.qualifiedName().toString(), value);
            }
            continue;
        }
        output.writeCurrentToken(input);
        if (input.isEndElement() && input.name() == QStringLiteral("spine"))
            inSpine = false;
    }
    if (input.hasError() || !selected)
        return fail(strings::get("error.could_not_isolate_the_current_section"));
    if (resourceError)
        return fail(strings::get("error.could_not_read_the_section_metadata"));
    // Change only the package metadata. Every other stream comes from the original
    // container, so opening a preview never copies the book's assets.
    auto preview = std::make_shared<ReaderSource>();
    preview->parent = source.source_;
    preview->resources = source.source_->resources;
    preview->container = LVContainerRef(
            new SectionContainer(container, cre(packagePath), std::move(previewPackage)));
    auto settings = source.state();
    QString anchor = source.position();
    anchor.replace(QRegularExpression("/DocFragment(?:\\[\\d+\\])?"), "/DocFragment[1]");
    settings["position"] = anchor;
    userStyleSheet_ = source.userStyleSheet_;
    return openSource(std::move(preview), size, settings, error);
}
} // namespace kobalt
