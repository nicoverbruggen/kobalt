#include "support/qt_compat.h"
#include "support/strings.h"
#include "reader_model.h"
#include "reader_source.h"
#include "kobo_styles.h"
#include "reader_render_callback.h"
#include "reader_alignment.h"
#include "reader_page_layout.h"
#include "document_cache.h"
#include "engine_activity.h"
#include "publication/publication.h"
#include <lvdocview.h>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <textlang.h>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QUrl>
#include <QXmlStreamWriter>
#include <QtEndian>
#include <climits>

namespace kobalt {
namespace {
QString qstring(const lString32 &s) {
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}
lString32 cre(const QString &s) {
    return Utf8ToUnicode(s.toUtf8().constData());
}
constexpr int maxBook = 64 * 1024 * 1024;
class ResourceStream : public LVNamedStream {
public:
    ResourceStream(std::shared_ptr<ResourceCache> resources, QString name, qint64 length, QSize dimensions = {})
        : resources(std::move(resources)), name(std::move(name)), length(length), dimensions(dimensions) {
        SetName(cre(this->name).c_str());
    }
    lvopen_mode_t GetMode() override {
        return LVOM_READ;
    }
    LVImageSource *CreateImageSource(ldomDocument *doc, ldomNode *node) override;
    lvsize_t GetSize() override {
        resolveLength();
        return lvsize_t(qMax(qint64(0), length));
    }
    lverror_t GetSize(lvsize_t *size) override {
        if (!resolveLength())
            return LVERR_FAIL;
        *size = lvsize_t(length);
        return LVERR_OK;
    }
    lverror_t Seek(lvoffset_t offset, lvseek_origin_t origin, lvpos_t *result) override {
        if (!resolveLength())
            return LVERR_FAIL;
        const qint64 base = origin == LVSEEK_SET ? 0 : origin == LVSEEK_CUR ? position : length;
        const qint64 next = base + offset;
        if ((origin != LVSEEK_SET && origin != LVSEEK_CUR && origin != LVSEEK_END) || next < 0 || next > length)
            return LVERR_FAIL;
        position = next;
        if (result)
            *result = lvpos_t(next);
        return LVERR_OK;
    }
    lverror_t Read(void *buffer, lvsize_t count, lvsize_t *read) override {
        if (read)
            *read = 0;
        if (!resolveLength())
            return LVERR_FAIL;
        const auto result = resources->read(name, position, buffer, qMin(qint64(count), length - position));
        if (result < 0)
            return LVERR_FAIL;
        position += result;
        if (read)
            *read = lvsize_t(result);
        return LVERR_OK;
    }
    lverror_t SetSize(lvsize_t) override {
        return LVERR_FAIL;
    }
    lverror_t Write(const void *, lvsize_t, lvsize_t *written) override {
        if (written)
            *written = 0;
        return LVERR_FAIL;
    }
    bool Eof() override {
        return !resolveLength() || position == length;
    }

private:
    std::shared_ptr<ResourceCache> resources;
    QString name;
    qint64 length, position = 0;
    QSize dimensions;
    bool resolveLength() {
        if (length < 0)
            length = resources->size(name);
        return length >= 0;
    }
};
class DeferredImage : public LVImageSource {
public:
    DeferredImage(std::shared_ptr<ResourceCache> resources, QString name, QSize size, ldomDocument *doc, ldomNode *node)
        : resources(std::move(resources)), name(std::move(name)), size(size), doc(doc), node(node) {}
    ldomDocument *GetSourceDocument() override {
        return doc;
    }
    ldomNode *GetSourceNode() override {
        return node;
    }
    LVStream *GetSourceStream() override {
        return nullptr;
    }
    void Compact() override {}
    int GetWidth() const override {
        return size.width();
    }
    int GetHeight() const override {
        return size.height();
    }
    bool Decode(LVImageDecoderCallback *callback) override {
        // Layout asks for dimensions with no callback. Only a paint requests
        // the pixels, so images on unread pages remain deferred.
        if (!callback)
            return true;
        if (resources->imageReady(name)) {
            const auto length = resources->size(name);
            if (length < 0)
                return false;
            auto image = LVCreateStreamImageSource(LVStreamRef(new ResourceStream(resources, name, length)), doc, node);
            if (image.isNull() || image->GetWidth() != size.width() || image->GetHeight() != size.height())
                return LVCreateDummyImageSource(node, size.width(), size.height())->Decode(callback);
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
LVImageSource *ResourceStream::CreateImageSource(ldomDocument *doc, ldomNode *node) {
    return dimensions.isValid() ? new DeferredImage(resources, name, dimensions, doc, node) : nullptr;
}
class ResourceContainer : public LVNamedContainer {
public:
    ResourceContainer(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes)
        : resources(std::move(resources)) {
        SetName(U"publication.epub");
        for (const auto &name : this->resources->names()) {
            auto *item = new LVCommonContainerItemInfo;
            item->SetItemInfo(cre(name), lvsize_t(sizes.value(name)), 0);
            Add(item);
        }
    }
    LVContainer *GetParentContainer() override {
        return nullptr;
    }
    lverror_t GetSize(lvsize_t *size) override {
        *size = GetObjectCount();
        return LVERR_OK;
    }
    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override {
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
class ContentContainer : public LVContainer {
public:
    explicit ContentContainer(LVContainerRef source) : source(source) {}
    bool IsContainer() override {
        return true;
    }
    const lChar32 *GetName() override {
        return source->GetName();
    }
    LVContainer *GetParentContainer() override {
        return source->GetParentContainer();
    }
    lverror_t GetSize(lvsize_t *size) override {
        return source->GetSize(size);
    }
    int GetObjectCount() const override {
        return source->GetObjectCount();
    }
    const LVContainerItemInfo *GetObjectInfo(int index) override {
        return source->GetObjectInfo(index);
    }
    const LVContainerItemInfo *GetObjectInfo(lString32 name) override {
        return source->GetObjectInfo(name);
    }
    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override {
        auto stream = source->OpenStream(name, mode);
        const auto suffix = QFileInfo(qstring(lString32(name))).suffix().toLower();
        // Bound the temporary chapter copy; leave large or non-text resources streamed.
        constexpr lvsize_t limit = 2 * 1024 * 1024;
        if (stream.isNull() || mode != LVOM_READ ||
            !(suffix.isEmpty() || suffix == "html" || suffix == "xhtml" || suffix == "htm" || suffix == "xml") ||
            stream->GetSize() > limit) {
            return stream;
        }
        QByteArray bytes(int(stream->GetSize()), '\0');
        lvsize_t read = 0;
        if (stream->Read(bytes.data(), bytes.size(), &read) != LVERR_OK || read != lvsize_t(bytes.size())) {
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
};
class SectionContainer : public ContentContainer {
public:
    SectionContainer(LVContainerRef source, lString32 packagePath, QByteArray package)
        : ContentContainer(source), packagePath(packagePath), package(std::move(package)) {
        packageInfo.SetItemInfo(packagePath, this->package.size(), 0);
    }
    const LVContainerItemInfo *GetObjectInfo(int index) override {
        const auto *item = source->GetObjectInfo(index);
        return item && lString32(item->GetName()) == packagePath ? &packageInfo : item;
    }
    const LVContainerItemInfo *GetObjectInfo(lString32 name) override {
        return name == packagePath ? &packageInfo : source->GetObjectInfo(name);
    }
    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) override {
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

}
bool ReaderModel::supportedArchive(const QByteArray &bytes, QString *error) {
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
    return true;
}

bool ReaderModel::open(QByteArray bytes, QSize size, const QJsonObject &saved, QString *error,
                       OpeningProgress progress) {
    if (!supportedArchive(bytes, error))
        return false;
    auto source = std::make_shared<ReaderSource>();
    source->bytes = std::move(bytes);
    source->container =
        LVOpenArchieve(LVCreateMemoryStream(source->bytes.data(), source->bytes.size(), false, LVOM_READ));
    if (source->container.isNull()) {
        *error = strings::get("error.the_reading_engine_could_not_open_this_epub");
        return false;
    }
    return openSource(std::move(source), size, saved, error, std::move(progress));
}
bool ReaderModel::openResources(std::shared_ptr<ResourceCache> resources, const QMap<QString, qint64> &sizes,
                                QSize size, const QJsonObject &saved, QString *error, OpeningProgress progress,
                                OpeningResult *result) {
    auto source = std::make_shared<ReaderSource>();
    source->resources = std::move(resources);
    source->sizes = sizes;
    source->container = LVContainerRef(new ResourceContainer(source->resources, sizes));
    return openSource(std::move(source), size, saved, error, std::move(progress), result);
}
bool ReaderModel::rebindResources(const std::shared_ptr<ResourceCache> &resources, const QMap<QString, qint64> &sizes) {
    // Previews retain ReaderSource as their parent. Do not change their access
    // underneath them, even if the main reader itself has already closed.
    if (!source_ || source_.use_count() != 1 || !source_->resources || source_->sizes != sizes || !resources)
        return false;
    return source_->resources->rebind(*resources);
}
QString ReaderModel::resourceError() const {
    return source_ && source_->resources ? source_->resources->error() : QString();
}
QJsonObject ReaderModel::resourceStats() const {
    auto result = source_ && source_->resources ? source_->resources->statistics() : QJsonObject();
    for (auto at = openingTimes_.begin(); at != openingTimes_.end(); ++at)
        result.insert(at.key(), at.value());
    return result;
}
bool ReaderModel::openSource(std::shared_ptr<ReaderSource> source, QSize size, const QJsonObject &saved, QString *error,
                             OpeningProgress progress, OpeningResult *result) {
    if (result)
        *result = {};
    const auto fail = [error, result](OpeningFailureKind kind, const char *reason, const QString &detail = QString()) {
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
    if (mime.isNull() || mime->GetSize() != lvsize_t(mediaType.size()) ||
        mime->Read(mediaType.data(), mediaType.size(), &read) != LVERR_OK || read != lvsize_t(mediaType.size()) ||
        mediaType != expectedType) {
        if (source->resources && !source->resources->error().isEmpty())
            return fail(OpeningFailureKind::Io, "opening.resource-read", source->resources->error());
        return fail(OpeningFailureKind::Unsupported, strings::key("error.the_publication_is_not_an_epub"));
    }
    if (!initializeFonts())
        return fail(OpeningFailureKind::HostUnavailable, strings::key("error.the_reading_font_is_unavailable"));
    configure(size, saved);
    openedFromCache_ = false;
    // Stream loading does not set a filename. CREngine uses its presence to
    // distinguish a loaded document from its internal blank/error document,
    // and suppresses pagination callbacks for the latter. With a fingerprint,
    // the name and checksum also select this book's cache file.
    auto properties = engine_->getDocProps();
    const bool keyed = !cacheDirectory_.isEmpty() && !cacheFingerprint_.isEmpty();
    if (keyed) {
        properties->setString(DOC_PROP_FILE_NAME,
                              Utf8ToUnicode(lString8(documentCacheName(cacheFingerprint_).toUtf8().constData())));
        properties->setHex(
            DOC_PROP_FILE_CRC32,
            documentCacheChecksum(documentCachePolicy() + ":bionic-v3:" + bionic_.toUtf8(), domVersion_));
        properties->setString(DOC_PROP_FILE_SIZE, lString32::itoa(int(cacheBytes_ < 0         ? 0
                                                                      : cacheBytes_ > INT_MAX ? INT_MAX
                                                                                              : cacheBytes_)));
    } else
        properties->setString(DOC_PROP_FILE_NAME, lString32(U"publication.epub"));
    if (!source->parent) {
        source->container = LVContainerRef(new ContentContainer(source->container));
    }
    OpeningCallback callback(engine_, std::move(progress));
    source_ = std::move(source);
    openingTimes_["model_prepare_ms"] = double(phase.restart());
    bool loaded;
    {
        DocumentCacheScope cache(keyed ? cacheDirectory_ : QString());
        loaded = engine_->LoadDocument(LVCreateMemoryStream(), false, source_->container);
        // A parsed document is rendered only after Render(); one restored
        // from the cache arrives rendered.
        openedFromCache_ = loaded && cache.enabled() && engine_->getDocument()->isRendered();
    }
    openingTimes_["model_cache_hit"] = openedFromCache_;
    if (!loaded) {
        const auto resourceFailure = resourceError();
        engine_.reset();
        return resourceFailure.isEmpty() ? fail(OpeningFailureKind::InvalidContent,
                                                strings::key("error.the_reading_engine_could_not_open_this_epub"))
                                         : fail(OpeningFailureKind::Io, "opening.resource-read", resourceFailure);
    }
    source_->embeddedFonts = source_->parent ? source_->parent->embeddedFonts : embeddedFontCount();
    applyPublisherFontFallbacks();
    engine_->getDocument()->getProps()->setInt("kobalt.bionic.mode", bionic_ == "bold" ? 1 : 0);
    openingTimes_["model_parse_ms"] = double(phase.restart());
    // The package language selects hyphenation patterns and line-breaking
    // rules for text without its own lang attribute.
    if (const auto language = engine_->getLanguage().trim(); !language.empty()) {
        auto properties = LVCreatePropsContainer();
        properties->setString(PROP_TEXTLANG_MAIN_LANG, language);
        engine_->propsApply(properties);
    }
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
        markChapterLeadingImages(engine_.get());
        markKoboLinkDecorations();
        // Loading computed styles before the body markers existed, including in previews.
        engine_->getDocument()->forceReinitStyles();
    }
    applyStyleSheet();
    engine_->Render();
    openingTimes_["model_paginate_ms"] = double(phase.restart());
    if (!resourceError().isEmpty()) {
        const auto detail = resourceError();
        engine_.reset();
        return fail(OpeningFailureKind::Io, "opening.resource-read", detail);
    }
    if (!pages()) {
        engine_.reset();
        return fail(OpeningFailureKind::InvalidContent, strings::key("error.the_publication_has_no_pages"));
    }
    engine_->getCurPage();
    anchor_ = qstring(engine_->getBookmark().toString());
    const auto bookmark = saved.value("position").toString();
    if (!bookmark.isEmpty() && !goTo(bookmark)) {
        engine_.reset();
        return fail(OpeningFailureKind::InvalidContent, strings::key("error.saved_reading_position_no_longer_exists"));
    }
    return true;
}
bool ReaderModel::openPreview(ReaderModel &source, QSize size, QString *error) {
    auto fail = [error](const QString &message) {
        *error = message;
        return false;
    };
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
        if (stream->Read(data.data(), data.size(), &count) != LVERR_OK || count != lvsize_t(data.size())) {
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
            const auto href = QUrl::fromPercentEncoding(manifest.attributes().value("href").toUtf8());
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
        output.writeCurrentToken(input);
        if (input.isEndElement() && input.name() == QStringLiteral("spine"))
            inSpine = false;
    }
    if (input.hasError() || !selected)
        return fail(strings::get("error.could_not_isolate_the_current_section"));
    if (resourceError)
        return fail(strings::get("error.could_not_read_the_section_metadata"));
    // Change only the spine. Every other stream comes from the original
    // container, so opening a preview never copies the book's assets.
    auto preview = std::make_shared<ReaderSource>();
    preview->parent = source.source_;
    preview->resources = source.source_->resources;
    preview->container = LVContainerRef(new SectionContainer(container, cre(packagePath), std::move(previewPackage)));
    auto settings = source.state();
    QString anchor = source.position();
    anchor.replace(QRegularExpression("/DocFragment(?:\\[\\d+\\])?"), "/DocFragment[1]");
    settings["position"] = anchor;
    userStyleSheet_ = source.userStyleSheet_;
    return openSource(std::move(preview), size, settings, error);
}
}
