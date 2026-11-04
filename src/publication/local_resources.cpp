#include "support/diagnostics.h"
#include "local_resources.h"

#include "image_policy.h"
#include "publication.h"
#include "support/strings.h"
#include "zip_resource.h"

#include <QElapsedTimer>
#include <QFile>
#include <QXmlStreamReader>

namespace kobalt {
namespace {
class LocalResources {
public:
    QString path, systemFont;
    QMap<QString, qint64> sizes;
    std::shared_ptr<SourceLease> lease;
    std::shared_ptr<std::atomic_bool> contentRejected;
    // ResourceCache access runs serially on the engine worker.
    ZipResourceIndex index;
    qint64 requests = 0, failures = 0, readNs = 0, sinkNs = 0, xmlNs = 0;

    bool cancelled() const {
        return !lease || lease->revoked();
    }
    QJsonObject statistics() const {
        return qt::object({{"requests", double(requests)},
                           {"request_failures", double(failures)},
                           {"request_ms", double(readNs) / 1000000.0},
                           {"resource_sink_ms", double(sinkNs) / 1000000.0},
                           {"resource_xml_ms", double(xmlNs) / 1000000.0}});
    }
    bool read(const QString &name, const ResourceCache::Sink &sink, QString *error, QSize *dimensions = nullptr) {
        QElapsedTimer timer;
        timer.start();
        ++requests;
        const bool ok = transfer(name, sink, error, dimensions);
        readNs += timer.nsecsElapsed();
        if (!ok) {
            ++failures;
        }
        return ok;
    }

private:
    bool transfer(const QString &name, const ResourceCache::Sink &sink, QString *error, QSize *dimensions) {
        auto fail = [&](const char *reason) {
            if (error && error->isEmpty()) {
                *error = strings::get(reason);
            }
            return false;
        };
        if (cancelled()) {
            return fail("error.reading_session_closed");
        }
        if (!sizes.contains(name)) {
            return fail("error.could_not_load_a_book_resource");
        }
        const bool image = imageFileName(name);
        if (image && sizes.value(name) > ResourceCache::budgetBytes) {
            const auto placeholder = imagePlaceholder({}, systemFont);
            return !cancelled() && sink(placeholder.constData(), placeholder.size());
        }
        ZipResource stream;
        if (!stream.open(path, name, index, ResourceCache::budgetBytes)) {
            return fail("error.could_not_load_a_book_resource");
        }
        if (stream.size() != sizes.value(name)) {
            return fail("error.a_book_resource_changed_during_reading");
        }
        QElapsedTimer deadline;
        deadline.start();
        const bool validateXml = name.endsWith(".opf", Qt::CaseInsensitive) || name == "META-INF/container.xml";
        QXmlStreamReader xml;
        auto deliver = [&](const QByteArray &bytes) {
            QElapsedTimer work;
            work.start();
            const bool delivered = !cancelled() && sink(bytes.constData(), bytes.size());
            sinkNs += work.nsecsElapsed();
            if (!delivered) {
                return false;
            }
            if (validateXml) {
                work.restart();
                xml.addData(bytes);
                while (!xml.atEnd()) {
                    xml.readNext();
                }
                xmlNs += work.nsecsElapsed();
                if (xml.hasError() && xml.error() != QXmlStreamReader::PrematureEndOfDocumentError) {
                    if (contentRejected) {
                        contentRejected->store(true);
                    }
                    return fail(strings::key("error.publication_package_xml_is_invalid"));
                }
            }
            return true;
        };
        QByteArray prefix;
        bool inspected = !image;
        while (stream.valid() && !stream.finished()) {
            if (cancelled()) {
                return fail("error.reading_session_closed");
            }
            if (deadline.elapsed() >= 5000) {
                return fail("error.native_publication_transfer_timed_out");
            }
            const auto bytes = stream.next();
            if (!stream.valid()) {
                return fail("error.could_not_load_a_book_resource");
            }
            if (!inspected) {
                prefix += bytes;
                const auto header = inspectImageHeader(prefix);
                if (header.action == ImageHeader::More) {
                    continue;
                }
                if (dimensions) {
                    if (header.action == ImageHeader::Keep) {
                        *dimensions = header.size;
                    }
                    return !cancelled();
                }
                if (header.action == ImageHeader::Replace) {
                    const auto placeholder = imagePlaceholder(header.size, systemFont);
                    return !cancelled() && sink(placeholder.constData(), placeholder.size());
                }
                inspected = true;
                if (!deliver(prefix)) {
                    return false;
                }
                prefix.clear();
            } else if (!bytes.isEmpty() && !deliver(bytes)) {
                return false;
            }
        }
        if (cancelled()) {
            return fail("error.reading_session_closed");
        }
        if (!stream.valid() || !stream.finished()) {
            return fail("error.could_not_load_a_book_resource");
        }
        if (!inspected && !prefix.isEmpty() && !deliver(prefix)) {
            return false;
        }
        if (validateXml && (xml.hasError() || xml.tokenType() != QXmlStreamReader::EndDocument)) {
            if (contentRejected) {
                contentRejected->store(true);
            }
            return fail(strings::key("error.publication_package_xml_is_invalid"));
        }
        return true;
    }
};
}

ResourceFactory localResourceFactory(QString path, std::shared_ptr<SourceLease> lease) {
    return [path = std::move(path), lease = std::move(lease)](const QMap<QString, qint64> &sizes,
                                                              const QString &systemFont,
                                                              std::shared_ptr<std::atomic_bool> contentRejected) {
        auto access = std::make_shared<LocalResources>();
        access->path = path;
        access->systemFont = systemFont;
        access->sizes = sizes;
        access->lease = lease;
        access->contentRejected = std::move(contentRejected);
        auto load = [access](const QString &name, const ResourceCache::Sink &sink, QString *error) {
            return access->read(name, sink, error);
        };
        auto probe = [access](const QString &name) {
            if (!imageFileName(name) || name.endsWith(".svg", Qt::CaseInsensitive) ||
                access->sizes.value(name) < 256 * 1024 || access->sizes.value(name) > ResourceCache::budgetBytes) {
                return QSize();
            }
            QSize dimensions;
            QString error;
            access->read(
                name,
                [](const char *, qint64) {
                    return true;
                },
                &error, &dimensions);
            return dimensions;
        };
        return std::make_shared<ResourceCache>(sizes.keys(), ResourceCache::Access{std::move(load),
                                                                                   [access] {
                                                                                       return access->cancelled();
                                                                                   },
                                                                                   [access] {
                                                                                       return access->statistics();
                                                                                   },
                                                                                   std::move(probe)});
    };
}

namespace native {
namespace {
class LocalPublication final : public PublicationEndpoint {
public:
    explicit LocalPublication(QString path) : path_(std::move(path)) {}
    int port() const override {
        return 0;
    }
    bool active() const override {
        return true;
    }
    ResourceFactory resources(std::shared_ptr<SourceLease> lease) const override {
        return localResourceFactory(path_, std::move(lease));
    }

private:
    QString path_;
};
}
std::unique_ptr<PublicationEndpoint> openLocalPublication(const QString &path, QString *error) {
    QFile file(path);
    Publication publication;
    if (!file.open(QIODevice::ReadOnly)) {
        reportDescriptorExhaustion("book-open");
        return {};
    }
    if (!publication.inspect(file, error))
        return {};
    return std::make_unique<LocalPublication>(path);
}
}
}
