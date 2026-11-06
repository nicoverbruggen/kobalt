// The fake Nickel functions of firmware 4, for the Qt5 adapter in
// platforms/kobo/nickel-qt5/. See nickel.h.
#include "nickel.h"
#include <QMetaType>
#include <QSqlQuery>

using pewter::record;

static_assert(sizeof(Volume) == 8, "firmware 4 Volume handle");

// The firmware 4 adapter looks the Volume size up by metatype name.
struct VolumeMetaType
{
    void *words[2];
};

[[maybe_unused]] static const int volumeMetaType = [] {
    return qRegisterMetaType<VolumeMetaType>("Volume");
}();

QSqlDatabase DatabaseAdapter::dbConnection(QString, bool)
{
    return pewter::nickelDatabase();
}

void VolumeManager::updateBookmark(Volume &volume, const QString &location, int adobeLocation,
                                   const QDateTime &date)
{
    record("VolumeManager::updateBookmark",
           { { "location", location }, { "adobeLocation", adobeLocation }, { "date", date } });
    pewter::storeBookmark(volume, location, date);
}

// Firmware 4 also stores the KEPUB chapter's percentage on the chapter's own
// row.
class ShortcoverManager
{
public:
    static void updatePercentRead(const QString &id, int percent, const QString &database);
};

void ShortcoverManager::updatePercentRead(const QString &id, int percent, const QString &database)
{
    record("ShortcoverManager::updatePercentRead",
           { { "id", id }, { "percent", percent }, { "database", database } });
    QSqlQuery query(pewter::nickelDatabase());
    query.prepare(QStringLiteral("UPDATE content SET ___PercentRead = ? WHERE ContentID = ?"));
    query.addBindValue(percent);
    query.addBindValue(id);
    query.exec();
}
