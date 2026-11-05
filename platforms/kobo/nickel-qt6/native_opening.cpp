#include "platforms/kobo/common/native_publication.h"
#include <QMetaType>
#include <QObject>
#include <cstring>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
QMetaType nativeType(const char *name) {
    return QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(name));
}
struct OpeningApi {
    QMetaType volume = nativeType("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE");
    QMetaType keys = nativeType("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI5QListI10ContentKeyEE8metaTypeE");
    QMetaType bookmark = nativeType("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI8BookmarkE8metaTypeE");
    QObject *(*create)(void *, const void *, const void *) =
        symbol<decltype(create)>("_ZN16ReadingViewMixin12createReaderERK6VolumeRKN20AbstractReaderOpener7ContextE");
    void (*open)(QObject *, const void *) =
        symbol<decltype(open)>("_ZN14N3ReaderOpener10openReaderERKN20AbstractReaderOpener7ContextE");
    bool valid() const {
        return volume.isValid() && volume.isCopyConstructible() && keys.sizeOf() == 12 && bookmark.sizeOf() == 8 &&
               keys.isCopyConstructible() && bookmark.isCopyConstructible() && create && open;
    }
};
const OpeningApi &openingApi() {
    static const OpeningApi instance;
    return instance;
}
}

struct NativeOpening::Data {
    // Firmware 5.18 Context layout: content keys, LibraryController*, Bookmark.
    alignas(void *) unsigned char context[24] = {};
    void *volume = nullptr;
    QPointer<QObject> library;
    bool keys = false, bookmark = false;
    ~Data() {
        const auto &api = openingApi();
        if (bookmark)
            api.bookmark.destruct(context + 16);
        if (keys)
            api.keys.destruct(context);
        if (volume)
            api.volume.destroy(volume);
    }
};
NativeOpening::NativeOpening() = default;
NativeOpening::~NativeOpening() = default;
bool NativeOpening::available() {
    return openingApi().valid();
}
bool NativeOpening::retain(const void *volume, const void *context) {
    data_.reset();
    if (!volume || !context || !available())
        return false;
    auto data = std::make_unique<Data>();
    const auto &api = openingApi();
    data->volume = api.volume.create(volume);
    data->keys = api.keys.construct(data->context, context) != nullptr;
    data->bookmark = api.bookmark.construct(data->context + 16, static_cast<const char *>(context) + 16) != nullptr;
    if (!data->volume || !data->keys || !data->bookmark)
        return false;
    void *library = nullptr;
    std::memcpy(&library, static_cast<const char *>(context) + 12, sizeof(library));
    if (library)
        data->library = reinterpret_cast<QObject *>(static_cast<char *>(library) + 12);
    std::memcpy(data->context + 12, &library, sizeof(library));
    data_ = std::move(data);
    return true;
}
std::unique_ptr<NativeOpening> NativeOpening::copy() const {
    if (!data_)
        return nullptr;
    // The stored context keeps the firmware layout. A library controller that
    // has gone away must not be retained again.
    alignas(void *) unsigned char context[24];
    std::memcpy(context, data_->context, sizeof(context));
    if (!data_->library)
        std::memset(context + 12, 0, sizeof(void *));
    auto result = std::make_unique<NativeOpening>();
    return result->retain(data_->volume, context) ? std::move(result) : nullptr;
}
const void *NativeOpening::volume() const {
    return data_ ? data_->volume : nullptr;
}
const void *NativeOpening::bookmark() const {
    return data_ ? data_->context + 16 : nullptr;
}
const void *NativeOpening::keys() const {
    return data_ ? data_->context : nullptr;
}
void NativeOpening::openStock() {
    if (!data_)
        return;
    if (!data_->library)
        std::memset(data_->context + 12, 0, sizeof(void *));
    // In the inspected firmware createReader does not access its mixin instance.
    // Construct a fresh opener instead of retaining a possibly dead mixin.
    const auto &api = openingApi();
    QPointer<QObject> opener = api.create(nullptr, data_->volume, data_->context);
    if (!opener)
        return;
    api.open(opener, data_->context);
    if (opener && !opener->parent())
        opener->deleteLater();
}

}
