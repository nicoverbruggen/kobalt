#pragma once
#include "platform/api/dictionary_service.h"
#include <QObject>
#include <QVector>
#include <QHash>
#include <memory>
#include <functional>
#include <algorithm>
#include <QLocale>
class QNetworkAccessManager;

namespace kobalt {
struct DownloadDictionary
{
    QString id, name, language, target, description, license, url;
    QByteArray sha256;
    qint64 size = 0;
};

struct DictionaryTransfer
{
    QString id;
    qint64 received = 0, total = 0;
    bool installing = false;
};
class StarDict;

// Downloads are asynchronous. Lookup remains synchronous and reads only the selected entry.
class DictionaryManager final : public QObject, public DictionaryService
{
    Q_OBJECT
public:
    // An injected network manager stays owned by the caller and must outlive this object.
    DictionaryManager(QString directory, std::unique_ptr<DictionaryService> fallback,
                      QObject *parent = nullptr, QNetworkAccessManager *network = nullptr);
    ~DictionaryManager() override;
    static QVector<DownloadDictionary> catalog();
    static QString normalizeLanguage(const QString &language);
    static QString languageName(const QString &language);
    static QVector<DownloadDictionary> orderedCatalog(QVector<DownloadDictionary> entries);
    QVector<DictionaryChoice> choices() const override;
    bool select(const QString &book, const QString &id, QString *error) override;
    DictionaryResult lookup(const QString &book, const QString &word) override;
    bool installed(const QString &id) const;
    bool busy() const;
    QString status() const;
    DictionaryTransfer transfer() const;
    QHash<QString, qint64> installedSizes() const;
    qint64 availableBytes() const;
    QString selectedDictionary(const QString &book) const;
    void setLanguage(const QString &language);
    void download(const QString &id);
    void cancel();
    bool remove(const QString &id, QString *error);
    // Also used by offline installer tests; the ZIP must match the supplied digest.
    static bool installArchive(const QString &directory, const DownloadDictionary &entry,
                               const QString &zip, QString *error,
                               const std::function<bool()> &cancelled = {});
signals:
    void changed();

private:
    struct Data;
    std::unique_ptr<Data> data;
};

inline QString DictionaryManager::normalizeLanguage(const QString &language)
{
    return language.trimmed().toLower().section('-', 0, 0).section('_', 0, 0);
}

inline QString DictionaryManager::languageName(const QString &language)
{
    const auto code = normalizeLanguage(language);
    if (code.isEmpty())
        return {};
    const QLocale locale(code);
    return locale.language() == QLocale::C ? code : QLocale::languageToString(locale.language());
}

inline QVector<DownloadDictionary>
DictionaryManager::orderedCatalog(QVector<DownloadDictionary> entries)
{
    std::sort(entries.begin(), entries.end(),
              [](const DownloadDictionary &left, const DownloadDictionary &right) {
                  const auto source =
                          QString::compare(languageName(left.language),
                                           languageName(right.language), Qt::CaseInsensitive);
                  if (source)
                      return source < 0;
                  const bool leftMono =
                          normalizeLanguage(left.language) == normalizeLanguage(left.target);
                  const bool rightMono =
                          normalizeLanguage(right.language) == normalizeLanguage(right.target);
                  if (leftMono != rightMono)
                      return leftMono;
                  const auto target =
                          QString::compare(languageName(left.target), languageName(right.target),
                                           Qt::CaseInsensitive);
                  return target ? target < 0 : left.id < right.id;
              });
    return entries;
}
} // namespace kobalt
