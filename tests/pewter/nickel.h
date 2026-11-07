#pragma once
// The fake Nickel classes. Each class and function has the name and the
// signature of the Nickel function that Kobalt's adapter resolves with dlsym,
// so the compiler gives it the same symbol name and calling convention. A
// signature that drifts from the adapter's lookup leaves the symbol
// unresolved, and the adapter's Api::ready() check fails the test. The test
// executable exports these symbols (ENABLE_EXPORTS).
//
// nickel.cpp defines what firmware 4, 5 and 6 share. nickel_qt5.cpp defines
// the firmware 4 functions and nickel_qt6.cpp the firmware 5 and 6 functions;
// each Kobo SDK builds one of them.
#include "pewter.h"
#include <QDateTime>
#include <QMap>
#include <QObject>
#include <QSqlDatabase>
#include <QString>
#include <QVariant>

struct VolumeData
{
    QString id;
    QVariantMap values;
};

enum ReadingStatus { };

class Content
{
public:
    QString getImageId() const;
    void setReadStatus(ReadingStatus status);
    void setFirstTimeReading(const QVariant &value);
    void setPercentRead(const QVariant &value);
    // Firmware 5 and 6 only.
    void setNumPages(const QVariant &value);
};

// Nickel's Volume is a handle of two words on firmware 4 and three on
// firmware 5 and 6. The adapter checks the size; each firmware file asserts
// it. The fake keeps its row in the first word.
class Volume : public Content
{
public:
    Volume() = default;

    explicit Volume(VolumeData *data) : data(data) { }

    Volume(const Volume &) = delete;
    Volume &operator=(const Volume &) = delete;
    ~Volume();
    QVariantMap getDbValues() const;
    void setRestOfBookEstimate(const QVariant &value);
    void setCurrentChapterEstimate(const QVariant &value);
    void setCurrentChapterProgress(const QVariant &value);

    VolumeData *data = nullptr;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void *unused[2] = {};
#else
    void *unused[1] = {};
#endif
};

class Device
{
public:
    static Device *getCurrentDevice();
    const QString &getDbName() const;
    bool isTolino() const;
};

class Image
{
public:
    static bool isDefaultCoverImage(const QString &imageId);
    static QString getFileName(const Device &device, const Volume &volume, const QString &type);
};

class DatabaseAdapter
{
public:
    // Firmware 4 takes the name by value, firmware 5 and 6 by reference.
    static QSqlDatabase dbConnection(QString name, bool create);
    static QSqlDatabase dbConnection(const QString &name, bool create);
    static bool beginTransaction(QString name);
    static bool endTransaction(QString name, bool commit);
};

class VolumeManager
{
public:
    static Volume getById(const QString &id, const QString &database);
    // Firmware 4.
    static void updateBookmark(Volume &volume, const QString &location, int adobeLocation,
                               const QDateTime &date);
    // Firmware 5 and 6 add the page, which Tolino devices use.
    static void updateBookmark(Volume &volume, const QString &location, int adobeLocation,
                               const QDateTime &date, int page);
    static bool updateFields(const QString &id, const QMap<QString, QVariant> &fields,
                             const QMap<QString, QVariant> &where, const QString &database);
};

namespace pewter {
void record(const char *name, QVariantMap arguments = {});
QSqlDatabase nickelDatabase();
QVariantMap loadContent(const QSqlDatabase &db, const QString &id);
bool transactionOpen();
void setTransactionOpen(bool open);

inline VolumeData &contentData(Content *content)
{
    return *static_cast<Volume *>(content)->data;
}

// What Nickel's updateBookmark does on every firmware: it sets the position
// and the time, in whole seconds, and marks the reading state for upload.
void storeBookmark(Volume &volume, const QString &location, const QDateTime &date);
} // namespace pewter
