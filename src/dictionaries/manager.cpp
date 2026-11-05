#include "manager.h"
#include "support/main_thread.h"
#include "stardict.h"
#include <dictionary_catalog.h>
#include "publication/zip_resource.h"
#include "storage/reader_storage.h"
#include "support/qt_compat.h"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QSaveFile>
#include <QRegularExpression>
#include <limits>
#include <sys/statvfs.h>

namespace kobalt {
namespace {
bool validId(const QString &id)
{
    return QRegularExpression("^[a-z]{2,3}-[a-z]{2,3}$").match(id).hasMatch();
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

QString folder(const QString &root, const QString &id)
{
    return QDir(root).filePath(id);
}

class InstallJob final : public QThread
{
public:
    QString root, zip, error;
    DownloadDictionary entry;
    bool ok = false;

    void run() override
    {
        ok = DictionaryManager::installArchive(root, entry, zip, &error,
                                               [this] { return isInterruptionRequested(); });
    }
};
} // namespace

struct DictionaryManager::Data
{
    QString root, message, activeId, language, downloadError;
    std::unique_ptr<DictionaryService> fallback;
    std::unique_ptr<StarDict> active;
    QHash<QString, QString> selected;
    QNetworkAccessManager network;
    QNetworkAccessManager *requests = &network;
    QPointer<QNetworkReply> reply;
    QTemporaryFile download;
    InstallJob *job = nullptr;
    QCryptographicHash hash{ QCryptographicHash::Sha256 };
    qint64 bytes = 0;
    int percent = -1;
    DownloadDictionary entry;
    bool stopped = false;
};

DictionaryManager::DictionaryManager(QString directory, std::unique_ptr<DictionaryService> fallback,
                                     QObject *parent, QNetworkAccessManager *network)
    : QObject(parent), data(std::make_unique<Data>())
{
    data->root = std::move(directory);
    data->fallback = std::move(fallback);
    if (network)
        data->requests = network;
}

DictionaryManager::~DictionaryManager()
{
    if (data->reply) {
        disconnect(data->reply, nullptr, this, nullptr);
        data->reply->abort();
    }
    if (data->job) {
        data->job->requestInterruption();
        data->job->wait();
        delete data->job;
    }
}

QVector<DownloadDictionary> DictionaryManager::catalog()
{
    QVector<DownloadDictionary> entries;
    for (const auto &value :
         QJsonDocument::fromJson(dictionaryCatalog).object()["dictionaries"].toArray()) {
        const auto d = value.toObject();
        DownloadDictionary entry;
        entry.id = d["id"].toString();
        entry.name = d["name"].toString();
        entry.language = d["language"].toString();
        entry.target = d["target"].toString();
        entry.description = d["description"].toString();
        entry.license = d["license"].toString();
        entry.url = d["url"].toString();
        entry.sha256 = QByteArray::fromHex(d["sha256"].toString().toLatin1());
        entry.size = qint64(d["size"].toDouble());
        if (validId(entry.id) && entry.sha256.size() == 32 && entry.size > 0
            && entry.size <= 256 * 1024 * 1024)
            entries.append(entry);
    }
    return entries;
}

bool DictionaryManager::installed(const QString &id) const
{
    if (!validId(id))
        return false;
    const auto directory = folder(data->root, id);
    const QFileInfo info(directory);
    if (info.isSymLink() || !info.isDir())
        return false;
    return storage::readJson(folder(directory, "installed.json"))["id"].toString() == id;
}

QVector<DictionaryChoice> DictionaryManager::choices() const
{
    QVector<DictionaryChoice> current, other;
    for (const auto &entry : orderedCatalog(catalog()))
        if (installed(entry.id)) {
            const DictionaryChoice choice{ "downloaded:" + entry.id, entry.name,
                                           normalizeLanguage(entry.language), false };
            (choice.language == data->language ? current : other).append(choice);
        }
    for (auto choice : data->fallback->choices()) {
        choice.fallback = true;
        choice.language = normalizeLanguage(choice.language);
        current.append(choice);
    }
    current += other;
    return current;
}

bool DictionaryManager::select(const QString &book, const QString &id, QString *error)
{
    if (id.startsWith("downloaded:")) {
        const auto pair = id.mid(11);
        if (!installed(pair))
            return fail(error, "This dictionary is not installed.");
        if (data->activeId != pair) {
            auto active = std::make_unique<StarDict>();
            if (!active->open(folder(data->root, pair), pair, error))
                return false;
            data->active = std::move(active);
            data->activeId = pair;
        }
    } else if (!data->fallback->select(book, id, error))
        return false;
    if (data->selected.value(book) == id)
        return true;
    data->selected.insert(book, id);
    emit changed();
    return true;
}

DictionaryResult DictionaryManager::lookup(const QString &book, const QString &word)
{
    const auto available = choices();
    auto id = data->selected.value(book);
    bool present = false;
    for (const auto &choice : available)
        present |= choice.id == id;
    if (!present) {
        data->selected.remove(book);
        id = available.isEmpty() ? QString() : available.first().id;
    }
    if (id.isEmpty())
        return data->fallback->lookup(book, word);
    QString error;
    if (!id.startsWith("downloaded:")) {
        if (!data->fallback->select(book, id, &error)) {
            DictionaryResult result;
            result.word = word;
            result.error = error;
            return result;
        }
        auto result = data->fallback->lookup(book, word);
        result.dictionaryId = id;
        if (result.dictionary.isEmpty())
            for (const auto &choice : available)
                if (choice.id == id) {
                    result.dictionary = choice.name;
                    break;
                }
        return result;
    }
    const auto pair = id.mid(11);
    if (data->activeId != pair) {
        auto active = std::make_unique<StarDict>();
        if (!active->open(folder(data->root, pair), pair, &error)) {
            DictionaryResult result;
            result.word = word;
            result.error = error;
            return result;
        }
        data->active = std::move(active);
        data->activeId = pair;
    }
    auto result = data->active->lookup(word);
    result.dictionaryId = id;
    for (const auto &entry : catalog())
        if (entry.id == pair) {
            result.dictionary = entry.name;
            result.language = entry.language;
            break;
        }
    return result;
}

bool DictionaryManager::busy() const
{
    return data->reply || data->job;
}

void DictionaryManager::setLanguage(const QString &language)
{
    data->language = normalizeLanguage(language);
}

QString DictionaryManager::status() const
{
    return data->message;
}

DictionaryTransfer DictionaryManager::transfer() const
{
    if (!busy())
        return {};
    return { data->entry.id, data->bytes, data->entry.size, bool(data->job) };
}

QHash<QString, qint64> DictionaryManager::installedSizes() const
{
    QHash<QString, qint64> result;
    for (const auto &entry : catalog())
        if (installed(entry.id)) {
            qint64 bytes = 0;
            // Installations are flat directories. Do not follow user-added symlinks.
            for (const auto &file : QDir(folder(data->root, entry.id))
                                            .entryInfoList(QDir::Files | QDir::NoDotAndDotDot))
                if (!file.isSymLink())
                    bytes += file.size();
            result.insert(entry.id, bytes);
        }
    return result;
}

qint64 DictionaryManager::availableBytes() const
{
    QFileInfo path(data->root);
    while (!path.exists()) {
        const auto parent = path.absolutePath();
        if (parent == path.absoluteFilePath())
            return -1;
        path.setFile(parent);
    }
    struct statvfs space{};
    if (::statvfs(QFile::encodeName(path.absoluteFilePath()).constData(), &space) != 0)
        return -1;
    const auto maximum = quint64(std::numeric_limits<qint64>::max());
    const auto blocks = quint64(space.f_bavail), size = quint64(space.f_frsize);
    if (size && blocks > maximum / size)
        return qint64(maximum);
    return qint64(blocks * size);
}

QString DictionaryManager::selectedDictionary(const QString &book) const
{
    const auto selected = data->selected.value(book);
    if (!selected.startsWith("downloaded:"))
        return {};
    const auto id = selected.mid(11);
    return installed(id) ? id : QString();
}

void DictionaryManager::cancel()
{
    data->stopped = true;
    if (data->reply)
        data->reply->abort();
    if (data->job)
        data->job->requestInterruption();
}

void DictionaryManager::download(const QString &id)
{
    if (busy() || installed(id))
        return;
    DownloadDictionary entry;
    for (const auto &candidate : catalog())
        if (candidate.id == id) {
            entry = candidate;
            break;
        }
    if (entry.id.isEmpty())
        return;
    if (QFileInfo(data->root).isSymLink() || !QDir().mkpath(data->root)) {
        data->message = "Cannot create dictionary storage.";
        emit changed();
        return;
    }
    data->download.close();
    data->download.remove();
    data->download.setFileTemplate(folder(data->root, ".download-XXXXXX"));
    if (!data->download.open()) {
        data->message = "Cannot save the download.";
        emit changed();
        return;
    }
    data->entry = entry;
    data->bytes = 0;
    data->percent = -1;
    data->hash.reset();
    data->stopped = false;
    data->downloadError.clear();
    data->message = "Downloading " + entry.name;
    const auto request = [this](const QUrl &url, int redirects, auto &&again) -> void {
        if (url.scheme() != "https" || redirects > 5) {
            data->message = "Unsafe download redirect.";
            data->download.remove();
            emit changed();
            return;
        }
        QNetworkRequest networkRequest(url);
        networkRequest.setRawHeader("User-Agent", "Kobalt dictionary downloader");
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
        networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                    QNetworkRequest::ManualRedirectPolicy);
#endif
        // Manual redirects also work on the Kobo Qt 5.2 build.
        data->reply = data->requests->get(networkRequest);
        const QPointer<QNetworkReply> reply = data->reply;
        auto *timeout = new QTimer(reply);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
        timeout->start(30000);
        reply->setReadBufferSize(65536);
        const auto drain = [this, reply, timeout] {
            timeout->start(30000);
            if (!reply || reply != data->reply || data->stopped || !data->downloadError.isEmpty()
                || reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid())
                return;
            while (reply && reply == data->reply && !data->stopped && data->downloadError.isEmpty()
                   && reply->bytesAvailable()) {
                const auto bytes = reply->read(65536);
                data->bytes += bytes.size();
                if (data->bytes > data->entry.size || data->download.write(bytes) != bytes.size()) {
                    data->downloadError = "Download size or storage error.";
                    reply->abort();
                    return;
                }
                data->hash.addData(bytes);
                const int percent = int(data->bytes * 100 / data->entry.size);
                if (percent != data->percent) {
                    data->percent = percent;
                    data->message = "Downloading " + data->entry.name + " · "
                            + QString::number(percent) + "%";
                    emit changed();
                }
            }
        };
        connect(reply, &QNetworkReply::readyRead, this, drain);
        auto retry = again;
        connect(reply, &QNetworkReply::finished, this, [this, reply, redirects, drain, retry] {
            if (!reply || reply != data->reply)
                return;
            // abort() can emit finished synchronously while drain() is reading.
            disconnect(reply, nullptr, this, nullptr);
            drain();
            const auto redirect =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            data->reply = nullptr;
            const auto url = reply->url();
            const auto error = reply->error();
            const auto errorText = reply->errorString();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply->deleteLater();
            if (!data->stopped && !redirect.isEmpty()) {
                retry(url.resolved(redirect), redirects + 1, retry);
                return;
            }
            if (data->stopped || !data->downloadError.isEmpty() || error != QNetworkReply::NoError
                || status != 200 || data->bytes != data->entry.size
                || data->hash.result() != data->entry.sha256) {
                data->message = data->stopped             ? "Download cancelled."
                        : !data->downloadError.isEmpty()  ? data->downloadError
                        : error != QNetworkReply::NoError ? errorText
                                                          : "Dictionary checksum or size differs.";
                data->download.close();
                data->download.remove();
                emit changed();
                return;
            }
            data->download.flush();
            data->download.close();
            data->message = "Installing " + data->entry.name;
            auto *job = new InstallJob;
            job->root = data->root;
            job->zip = data->download.fileName();
            job->entry = data->entry;
            data->job = job;
            connect(job, &QThread::finished, this, [this, job] {
                data->message = job->ok ? "Dictionary installed." : job->error;
                data->job = nullptr;
                data->download.remove();
                job->deleteLater();
                emit changed();
            });
            job->start();
            emit changed();
        });
        emit changed();
    };
    request(QUrl(entry.url), 0, request);
}

bool DictionaryManager::remove(const QString &id, QString *error)
{
    if (busy() && data->entry.id == id)
        return fail(error, "Wait for this download to finish.");
    if (!installed(id))
        return fail(error, "This dictionary is not installed.");
    if (data->activeId == id) {
        data->active.reset();
        data->activeId.clear();
    }
    if (!QDir(folder(data->root, id)).removeRecursively())
        return fail(error, "Cannot remove this dictionary.");
    for (auto it = data->selected.begin(); it != data->selected.end();) {
        if (it.value() == "downloaded:" + id)
            it = data->selected.erase(it);
        else
            ++it;
    }
    if (!busy())
        data->message = "Dictionary removed.";
    emit changed();
    return true;
}

bool DictionaryManager::installArchive(const QString &directory, const DownloadDictionary &entry,
                                       const QString &zip, QString *error,
                                       const std::function<bool()> &cancelled)
{
    if (!validId(entry.id) || entry.sha256.size() != 32 || QFileInfo(directory).isSymLink()
        || !QDir().mkpath(directory) || QFileInfo::exists(folder(directory, entry.id)))
        return fail(error, "Cannot install this dictionary.");
    QFile archive(zip);
    if (!archive.open(QIODevice::ReadOnly) || archive.size() != entry.size)
        return fail(error, "Dictionary download is incomplete.");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!archive.atEnd()) {
        if (cancelled && cancelled())
            return fail(error, "Installation cancelled.");
        hash.addData(archive.read(65536));
    }
    if (hash.result() != entry.sha256)
        return fail(error, "Dictionary checksum differs.");
    QTemporaryDir stage(folder(directory, ".install-XXXXXX"));
    if (!stage.isValid())
        return fail(error, "Cannot create dictionary storage.");
    const auto files = QStringList{ entry.id + ".ifo", entry.id + ".idx", entry.id + ".dict.dz",
                                    entry.id + ".syn", "LICENSE",         "ATTRIBUTION",
                                    "README.txt" };
    for (const auto &name : files) {
        ZipResource resource;
        if (!resource.open(zip, entry.id + "/" + name,
                           name.endsWith(".ifo") ? 65536
                                   : name.contains('.') && name != "README.txt"
                                   ? 256LL * 1024 * 1024
                                   : 1024 * 1024)) {
            if (name.endsWith(".syn") || name == "README.txt")
                continue;
            return fail(error, "Dictionary archive is missing required files.");
        }
        mainthread::BlockingOperation blockingOperation("dictionary-install-write");
        QFile output(folder(stage.path(), name));
        if (!output.open(QIODevice::WriteOnly))
            return fail(error, "Cannot save dictionary files.");
        while (resource.valid() && !resource.finished()) {
            if (cancelled && cancelled())
                return fail(error, "Installation cancelled.");
            const auto bytes = resource.next();
            if (output.write(bytes) != bytes.size())
                return fail(error, "Not enough space for this dictionary.");
        }
        if (!resource.valid())
            return fail(error, "Dictionary archive is damaged.");
    }
    if (!StarDict::prepare(stage.path(), entry.id, error, cancelled))
        return false;
    if (!storage::writeJson(
                folder(stage.path(), "installed.json"),
                qt::object({ { "id", entry.id },
                             { "name", entry.name },
                             { "sha256", QString::fromLatin1(entry.sha256.toHex()) } })))
        return fail(error, "Cannot save dictionary metadata.");
    if (cancelled && cancelled())
        return fail(error, "Installation cancelled.");
    if (!QDir().rename(stage.path(), folder(directory, entry.id)))
        return fail(error, "Cannot activate installed dictionary.");
    stage.setAutoRemove(false);
    return true;
}
} // namespace kobalt
