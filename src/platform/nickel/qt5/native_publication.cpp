#include "platform/nickel/common/native_publication.h"
#include "platform/nickel/common/native_ui.h"
#include "platform/nickel/common/publication_source.h"
#include "publication/native_resources.h"
#include "native_location.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QDir>
#include <QVariantMap>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
struct Api {
    QString (*identity)(const void *) = symbol<decltype(identity)>("_ZNK7Content5getIdEv");
    QVariantMap (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    QString (*directory)(const void *, const void *) =
        symbol<decltype(directory)>("_ZN13VolumeManager12getKepubPathERK6DeviceRK6Volume");
    QString (*bookmarkPath)(const void *) = symbol<decltype(bookmarkPath)>("_ZNK8Bookmark18getChapterFilePathEv");
    bool valid() const {
        return identity && values && device && directory && bookmarkPath;
    }
};
const Api &api() {
    static const Api value;
    return value;
}
}
bool NativePublication::available() {
    return native::publicationSource().available() && api().valid();
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

bool NativePublication::open(const void *volume, const void *bookmark, QString *error, const void *keys) {
    close();
    identity_.clear();
    path_.clear();
    location_.clear();
    coverPath_.clear();
    auto fail = [&](const char *stage, const QString &reason) {
        diagnostic("native-publication-failed", {{"stage", QString::fromLatin1(stage)}});
        if (error)
            *error = reason;
        return false;
    };
    if (!volume || !keys || !available())
        return fail("native_publication_api_is_unavailable",
                    strings::get("error.native_publication_api_is_unavailable"));
    const auto &native = api();
    identity_ = native.identity(volume);
    path_ = nickel4::localBookPath(identity_);
    if (path_.isEmpty()) {
        // Firmware 4's getKepubPath returns the directory, not the book path.
        if (identity_.isEmpty() || identity_.contains('/') || identity_.contains('\\') || identity_ == "." ||
            identity_ == "..")
            return fail("native_publication_identity_is_unsupported",
                        strings::get("error.native_publication_identity_is_unsupported"));
        const auto directory = native.directory(native.device(), volume);
        if (directory.isEmpty())
            return fail("native_publication_directory_is_unavailable",
                        strings::get("error.native_publication_directory_is_unavailable"));
        path_ = QDir(directory).filePath(identity_);
    }
    if (identity_.isEmpty() || path_.isEmpty())
        return fail("native_publication_identity_is_unavailable",
                    strings::get("error.native_publication_identity_is_unavailable"));
    // Library openings carry an empty bookmark. Explicit annotation locations
    // need a separate conversion from QtWebKit's container paths; keep those
    // requests in stock reading until that conversion is validated.
    if (bookmark && !native.bookmarkPath(bookmark).isEmpty())
        return fail("explicit_native_bookmark_is_unsupported",
                    strings::get("error.explicit_native_bookmark_is_unsupported"));
    location_ =
        nickel4::shared(nickel4::chapter(identity_, native.values(volume).value("ChapterIDBookmarked").toString()));
    source_ = native::publicationSource().open(volume, keys, path_, identity_, error);
    if (!source_)
        return fail("cannot_start_the_native_publication_service",
                    strings::get("error.cannot_start_the_native_publication_service"));
    port_ = source_->port();
    coverPath_ = nativeReaderCoverPath(volume);
    return true;
}
}
