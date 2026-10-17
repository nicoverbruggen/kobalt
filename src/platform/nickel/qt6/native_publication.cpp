#include "support/strings.h"
#include "platform/nickel/common/native_publication.h"
#include "platform/nickel/common/native_ui.h"
#include "platform/nickel/common/publication_source.h"
#include "publication/native_resources.h"
#include "platform/nickel/common/native_location.h"
#include <QMap>
#include <QVariant>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
// These native return values use caller-owned storage on ARM. Their native
// destructors retain responsibility for their reference-counted contents.
struct Location {
    void *words[2];
    Location(const Location &) = delete;
    ~Location() {
        symbol<void (*)(void *)>("_ZN15ContentLocationD1Ev")(this);
    }
};
static_assert(sizeof(Location) == 8);
struct Api {
    QString (*identity)(const void *, const QString &) = symbol<decltype(identity)>("_ZNK7Content5getIdERK7QString");
    QString (*path)(const void *) = symbol<decltype(path)>("_ZN13VolumeManager17getVolumeFilePathERK6Volume");
    QMap<QString, QVariant> (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    const Location *(*bookmark)(const void *) = symbol<decltype(bookmark)>("_ZNK8Bookmark16getStartLocationEv");
    Location (*location)(QString, QString) = symbol<decltype(location)>("_ZN15ContentLocation6createE7QStringS0_");
    QString (*locationString)(const Location *) =
        symbol<decltype(locationString)>("_ZNK15ContentLocation12toTENGStringEv");
    bool (*locationValid)(const Location *) = symbol<decltype(locationValid)>("_ZNK15ContentLocation7isValidEv");
    bool (*lcp)(const void *) = symbol<decltype(lcp)>("_ZNK7Content9isLcpEpubEv");
    bool valid() const {
        return identity && path && values && bookmark && location && locationString && locationValid && lcp &&
               symbol<void *>("_ZN15ContentLocationD1Ev");
    }
};
const Api &api() {
    static const Api instance;
    return instance;
}
}

bool NativePublication::available() {
    return api().valid() && native::publicationSource().available();
}
NativePublication::NativePublication() = default;
bool NativePublication::active() const {
    return source_ && source_->active();
}
ResourceFactory NativePublication::resources(std::shared_ptr<SourceLease> lease) const {
    if (!source_) {
        return {};
    }
    auto direct = source_->resources(lease);
    return direct ? direct : nativeResourceFactory(path_, port_, std::move(lease));
}
NativePublication::~NativePublication() {
    close();
}
void NativePublication::close() {
    source_.reset();
    port_ = 0;
}

bool NativePublication::open(const void *volume, const void *bookmark, QString *error, const void *openingKeys) {
    close();
    identity_.clear();
    path_.clear();
    location_.clear();
    coverPath_.clear();
    auto fail = [&](const QString &reason) {
        close();
        if (error)
            *error = reason;
        return false;
    };
    const auto &native = api();
    if (!volume || !native.valid() || !native::publicationSource().available())
        return fail(strings::get("error.native_publication_api_is_unavailable"));
    identity_ = native.identity(volume, QString());
    path_ = native.path(volume);
    if (identity_.isEmpty() || path_.isEmpty())
        return fail(strings::get("error.native_publication_identity_is_unavailable"));
    if (bookmark) {
        const auto *location = native.bookmark(bookmark);
        if (location && native.locationValid(location)) {
            location_ = publicationLocation(identity_, native.locationString(location));
            if (location_.isEmpty())
                return fail(strings::get("error.native_opening_position_cannot_be_converted"));
        }
    }
    if (location_.isEmpty()) {
        // Read the native volume snapshot, without opening or changing its DB.
        const auto saved = native.values(volume).value("ChapterIDBookmarked").toString();
        if (!saved.isEmpty()) {
            const auto location = native.location(publicationLocation(identity_, saved), QString());
            if (!native.locationValid(&location))
                return fail(strings::get("error.native_saved_position_is_unsupported"));
            location_ = native.locationString(&location);
            if (location_.isEmpty())
                return fail(strings::get("error.native_saved_position_cannot_be_converted"));
        }
    }
    // Eligibility already keeps LCP books in Nickel. Refuse them here too, so
    // no other opening path can start a native service for one.
    if (native.lcp(volume))
        return fail(strings::get("error.this_publication_is_lcp_protected"));
    source_ = native::publicationSource().open(volume, openingKeys, path_, identity_, error);
    if (!source_)
        return false;
    port_ = source_->port();
    coverPath_ = nativeReaderCoverPath(volume);
    return true;
}

}
