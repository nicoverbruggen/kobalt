// The fake Nickel functions of firmware 5 and 6, for the Qt6 adapter in
// platforms/kobo/nickel-qt6/. See nickel.h.
#include "nickel.h"
#include <QMetaType>

using pewter::record;

static_assert(sizeof(Volume) == 12, "firmware 5 and 6 Volume handle");

// The firmware 5 and 6 adapter checks the Volume size through Qt's metatype
// for a type named Volume. Taking its address emits that symbol.
extern const void *const pewterVolumeMetaType =
        &QtPrivate::QMetaTypeInterfaceWrapper<Volume>::metaType;

void Content::setNumPages(const QVariant &value)
{
    record("Content::setNumPages", { { "value", value } });
    pewter::contentData(this).values.insert("___NumPages", value);
}

QSqlDatabase DatabaseAdapter::dbConnection(const QString &, bool)
{
    return pewter::nickelDatabase();
}

void VolumeManager::updateBookmark(Volume &volume, const QString &location, int adobeLocation,
                                   const QDateTime &date, int page)
{
    record("VolumeManager::updateBookmark",
           { { "location", location },
             { "adobeLocation", adobeLocation },
             { "date", date },
             { "page", page } });
    pewter::storeBookmark(volume, location, date);
}

// Firmware 5 and 6 convert positions with ContentLocation. The fake keeps
// Kobalt's position as it is, so a position round-trips unchanged.
class ContentLocation
{
public:
    ContentLocation() = default;

    explicit ContentLocation(QString *value) : value(value) { }

    ContentLocation(const ContentLocation &) = delete;
    ContentLocation &operator=(const ContentLocation &) = delete;
    ~ContentLocation();
    static ContentLocation create(QString location, QString base);
    bool isValid() const;
    QString toTENGString() const;
    QString toString() const;

    QString *value = nullptr;
    void *unused = nullptr;
};

static_assert(sizeof(ContentLocation) == 8, "firmware 5 and 6 ContentLocation handle");

ContentLocation::~ContentLocation()
{
    delete value;
}

ContentLocation ContentLocation::create(QString location, QString)
{
    return ContentLocation(new QString(location));
}

bool ContentLocation::isValid() const
{
    return value && !value->isEmpty();
}

QString ContentLocation::toTENGString() const
{
    return value ? *value : QString();
}

QString ContentLocation::toString() const
{
    return value ? *value : QString();
}
