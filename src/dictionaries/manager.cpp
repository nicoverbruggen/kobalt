#include "manager.h"
#include "support/main_thread.h"
#include "support/strings.h"
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
#include <QEventLoop>
#include <atomic>
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
    QString root, result;
    DictionaryDownloadResult outcome = DictionaryDownloadResult::Failed;
    DownloadDictionary entry;
    DictionaryManager *owner;
    DictionaryManager::NetworkFactory networkFactory;
    std::function<void(qint64, bool)> progress;
    std::atomic_bool stopped{ false };

    void run() override
    {
        mainthread::blocking("dictionary download storage");
        if (QFileInfo(root).isSymLink() || !QDir().mkpath(root)) {
            result = strings::get("dictionaries.cannot_create_dictionary_storage");
            return;
        }
        QTemporaryFile file(folder(root, ".download-XXXXXX"));
        if (!file.open()) {
            result = strings::get("dictionaries.cannot_save_the_download");
            return;
        }
        auto network =
                networkFactory ? networkFactory() : std::make_unique<QNetworkAccessManager>();
        QUrl url(entry.url);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        qint64 received = 0;
        int lastPercent = -1;
        for (int redirects = 0; !stopped; ++redirects) {
            if (url.scheme() != "https" || redirects > 5) {
                result = strings::get("dictionaries.unsafe_download_redirect");
                return;
            }
            QNetworkRequest request(url);
            request.setRawHeader("User-Agent", "Kobalt dictionary downloader");
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::ManualRedirectPolicy);
#endif
            std::unique_ptr<QNetworkReply> reply(network->get(request));
            reply->setReadBufferSize(65536);
            QEventLoop loop;
            QTimer timeout(reply.get());
            timeout.setSingleShot(true);
            timeout.setInterval(30000);
            connect(&timeout, &QTimer::timeout, &loop, [&] {
                result = strings::get("dictionaries.download_timed_out");
                reply->abort();
            });
            connect(owner, &DictionaryManager::cancelDownload, reply.get(), &QNetworkReply::abort);
            connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
            auto drain = [&] {
                timeout.start();
                if (reply->attribute(QNetworkRequest::RedirectionTargetAttribute).isValid())
                    return;
                while (!stopped && result.isEmpty() && reply->bytesAvailable()) {
                    const auto bytes = reply->read(65536);
                    mainthread::blocking("dictionary download write");
                    if (bytes.isEmpty() || received + bytes.size() > entry.size
                        || file.write(bytes) != bytes.size()) {
                        result = strings::get("dictionaries.download_size_or_storage_error");
                        reply->abort();
                        return;
                    }
                    received += bytes.size();
                    hash.addData(bytes);
                    const int percent = int(received * 100 / entry.size);
                    if (percent != lastPercent) {
                        lastPercent = percent;
                        progress(received, false);
                    }
                }
            };
            connect(reply.get(), &QNetworkReply::readyRead, &loop, drain);
            timeout.start();
            if (!stopped && !reply->isFinished())
                loop.exec();
            if (stopped)
                break;
            drain();
            if (!result.isEmpty())
                return;
            const auto redirect =
                    reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            if (!redirect.isEmpty()) {
                url = reply->url().resolved(redirect);
                continue;
            }
            if (reply->error() != QNetworkReply::NoError) {
                result = reply->errorString();
                return;
            }
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
                || received != entry.size || hash.result() != entry.sha256) {
                result = strings::get("dictionaries.dictionary_checksum_or_size_differs");
                return;
            }
            if (!file.flush()) {
                result = strings::get("dictionaries.download_size_or_storage_error");
                return;
            }
            file.close();
            progress(received, true);
            if (DictionaryManager::installArchive(root, entry, file.fileName(), &result, [this] {
                    if (!stopped)
                        return false;
                    outcome = DictionaryDownloadResult::Cancelled;
                    return true;
                })) {
                outcome = DictionaryDownloadResult::Installed;
                result = strings::get("dictionaries.dictionary_installed");
            }
            return;
        }
        outcome = DictionaryDownloadResult::Cancelled;
        result = strings::get("dictionaries.download_cancelled");
    }
};
} // namespace

struct DictionaryManager::Data
{
    QString root, message, activeId, language;
    DictionaryDownloadResult result = DictionaryDownloadResult::None;
    std::unique_ptr<DictionaryService> fallback;
    std::unique_ptr<StarDict> active;
    QHash<QString, QString> selected;
    NetworkFactory network;
    InstallJob *job = nullptr;
    qint64 bytes = 0;
    bool installing = false;
    DownloadDictionary entry;
};

DictionaryManager::DictionaryManager(QString directory, std::unique_ptr<DictionaryService> fallback,
                                     QObject *parent, NetworkFactory network)
    : QObject(parent), data(std::make_unique<Data>())
{
    data->root = std::move(directory);
    data->fallback = std::move(fallback);
    data->network = std::move(network);
}

DictionaryManager::~DictionaryManager()
{
    if (data->job) {
        cancel();
        mainthread::blocking("dictionary download shutdown");
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
            return fail(error, strings::get("dictionaries.this_dictionary_is_not_installed"));
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
    return data->job != nullptr;
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
        return { {}, 0, 0, false, data->result };
    return { data->entry.id, data->bytes, data->entry.size, data->installing };
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
    if (data->job) {
        data->job->stopped = true;
        emit cancelDownload();
    }
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
    auto *job = new InstallJob;
    job->owner = this;
    job->root = data->root;
    job->entry = entry;
    job->networkFactory = data->network;
    job->progress = [this, job](qint64 bytes, bool installing) {
        qt::invoke(
                this,
                [this, job, bytes, installing] {
                    if (data->job != job || job->stopped)
                        return;
                    data->bytes = bytes;
                    data->installing = installing;
                    data->message = installing
                            ? strings::get("dictionaries.installing_name").arg(data->entry.name)
                            : strings::get("dictionaries.downloading_progress")
                                      .arg(data->entry.name,
                                           QString::number(bytes * 100 / data->entry.size));
                    emit changed();
                },
                Qt::QueuedConnection);
    };
    data->job = job;
    data->entry = entry;
    data->bytes = 0;
    data->installing = false;
    data->message = strings::get("dictionaries.downloading_name").arg(entry.name);
    connect(job, &QThread::finished, this, [this, job] {
        data->message = job->result;
        data->result = job->outcome;
        data->job = nullptr;
        job->deleteLater();
        emit changed();
    });
    job->start();
    emit changed();
}

bool DictionaryManager::remove(const QString &id, QString *error)
{
    if (busy() && data->entry.id == id)
        return fail(error, strings::get("dictionaries.wait_for_this_download_to_finish"));
    if (!installed(id))
        return fail(error, strings::get("dictionaries.this_dictionary_is_not_installed"));
    if (data->activeId == id) {
        data->active.reset();
        data->activeId.clear();
    }
    if (!QDir(folder(data->root, id)).removeRecursively())
        return fail(error, strings::get("dictionaries.cannot_remove_this_dictionary"));
    for (auto it = data->selected.begin(); it != data->selected.end();) {
        if (it.value() == "downloaded:" + id)
            it = data->selected.erase(it);
        else
            ++it;
    }
    if (!busy())
        data->message = strings::get("dictionaries.dictionary_removed");
    emit changed();
    return true;
}

bool DictionaryManager::installArchive(const QString &directory, const DownloadDictionary &entry,
                                       const QString &zip, QString *error,
                                       const std::function<bool()> &cancelled)
{
    if (!validId(entry.id) || entry.sha256.size() != 32 || QFileInfo(directory).isSymLink()
        || !QDir().mkpath(directory) || QFileInfo::exists(folder(directory, entry.id)))
        return fail(error, strings::get("dictionaries.cannot_install_this_dictionary"));
    QFile archive(zip);
    if (!archive.open(QIODevice::ReadOnly) || archive.size() != entry.size)
        return fail(error, strings::get("dictionaries.dictionary_download_is_incomplete"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!archive.atEnd()) {
        if (cancelled && cancelled())
            return fail(error, strings::get("dictionaries.installation_cancelled"));
        hash.addData(archive.read(65536));
    }
    if (hash.result() != entry.sha256)
        return fail(error, strings::get("dictionaries.dictionary_checksum_differs"));
    QTemporaryDir stage(folder(directory, ".install-XXXXXX"));
    if (!stage.isValid())
        return fail(error, strings::get("dictionaries.cannot_create_dictionary_storage"));
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
            return fail(error,
                        strings::get("dictionaries.dictionary_archive_is_missing_required_files"));
        }
        mainthread::BlockingOperation blockingOperation("dictionary-install-write");
        QFile output(folder(stage.path(), name));
        if (!output.open(QIODevice::WriteOnly))
            return fail(error, strings::get("dictionaries.cannot_save_dictionary_files"));
        while (resource.valid() && !resource.finished()) {
            if (cancelled && cancelled())
                return fail(error, strings::get("dictionaries.installation_cancelled"));
            const auto bytes = resource.next();
            if (output.write(bytes) != bytes.size())
                return fail(error,
                            strings::get("dictionaries.not_enough_space_for_this_dictionary"));
        }
        if (!resource.valid())
            return fail(error, strings::get("dictionaries.dictionary_archive_is_damaged"));
    }
    if (!StarDict::prepare(stage.path(), entry.id, error, cancelled))
        return false;
    if (!storage::writeJson(
                folder(stage.path(), "installed.json"),
                qt::object({ { "id", entry.id },
                             { "name", entry.name },
                             { "sha256", QString::fromLatin1(entry.sha256.toHex()) } })))
        return fail(error, strings::get("dictionaries.cannot_save_dictionary_metadata"));
    if (cancelled && cancelled())
        return fail(error, strings::get("dictionaries.installation_cancelled"));
    if (!QDir().rename(stage.path(), folder(directory, entry.id)))
        return fail(error, strings::get("dictionaries.cannot_activate_installed_dictionary"));
    stage.setAutoRemove(false);
    return true;
}
} // namespace kobalt
