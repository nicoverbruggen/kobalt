#include "opening_context.h"
#include "platforms/kobo/common/publication_source.h"
#include "platforms/kobo/common/native_publication.h"
#include <QObject>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T>
T symbol(const char *name)
{
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

struct Api
{
    void *(*copyVolume)(const void *) = symbol<decltype(copyVolume)>(
            "_ZN17QtMetaTypePrivate23QMetaTypeFunctionHelperI6VolumeLb1EE6CreateEPKv");
    void (*deleteVolume)(void *) = symbol<decltype(deleteVolume)>(
            "_ZN17QtMetaTypePrivate23QMetaTypeFunctionHelperI6VolumeLb1EE6DeleteEPv");
    void *(*copyKeys)(void *, const void *) =
            symbol<decltype(copyKeys)>("_ZN7QVectorI10ContentKeyEC1ERKS1_");
    void (*destroyKeys)(void *) = symbol<decltype(destroyKeys)>("_ZN7QVectorI10ContentKeyED1Ev");
    void *(*bookmark)(void *) = symbol<decltype(bookmark)>("_ZN8BookmarkC1Ev");
    void *(*assignBookmarkData)(void *, const void *) = symbol<decltype(assignBookmarkData)>(
            "_ZN18QSharedDataPointerI15BookmarkPrivateEaSERKS1_");
    void (*destroyBookmark)(void *) = symbol<decltype(destroyBookmark)>("_ZN8BookmarkD1Ev");
    QObject *(*create)(void *, const void *, const void *, void *) = symbol<decltype(create)>(
            "_ZN16ReadingViewMixin12createReaderERK6VolumeRK7QVectorI10ContentKeyEP25AbstractLibraryControllerIS0_E");
    void (*open)(QObject *, const void *) =
            symbol<decltype(open)>("_ZN14N3ReaderOpener10openReaderERK8Bookmark");

    bool valid() const
    {
        return copyVolume && deleteVolume && copyKeys && destroyKeys && bookmark
                && assignBookmarkData && destroyBookmark && create && open;
    }
};

const Api &api()
{
    static const Api value;
    return value;
}
} // namespace

struct NativeOpening::Data
{
    void *volume = nullptr;
    alignas(void *) unsigned char keys[4] = {};
    alignas(void *) unsigned char bookmark[8] = {};
    QPointer<QObject> library;
    void *libraryController = nullptr;
    bool copiedKeys = false, constructedBookmark = false;

    ~Data()
    {
        const auto &native = api();
        if (constructedBookmark)
            native.destroyBookmark(bookmark);
        if (copiedKeys)
            native.destroyKeys(keys);
        if (volume)
            native.deleteVolume(volume);
    }
};

NativeOpening::NativeOpening() = default;
NativeOpening::~NativeOpening() = default;

bool NativeOpening::available()
{
    return native::publicationSource().available() && api().valid();
}

bool NativeOpening::retain(const void *volume, const void *context)
{
    data_.reset();
    if (!available() || !volume || !context)
        return false;
    const auto &request = *static_cast<const nickel_qt5::OpeningContext *>(context);
    if (!request.keys || !request.bookmark)
        return false;
    auto data = std::make_unique<Data>();
    const auto &native = api();
    data->volume = native.copyVolume(volume);
    if (!data->volume)
        return false;
    native.copyKeys(data->keys, request.keys);
    data->copiedKeys = true;
    native.bookmark(data->bookmark);
    data->constructedBookmark = true;
    // Firmware 4.46 layout: Bookmark is an ScObject vtable followed by its shared
    // private data. Native assignment releases the default data and retains
    // every field of the original bookmark, including transient values.
    native.assignBookmarkData(data->bookmark + 4, static_cast<const char *>(request.bookmark) + 4);
    data->libraryController = request.library;
    if (request.library)
        data->library = reinterpret_cast<QObject *>(static_cast<char *>(request.library) + 12);
    data_ = std::move(data);
    return true;
}

std::unique_ptr<NativeOpening> NativeOpening::copy() const
{
    if (!data_)
        return nullptr;
    // A library controller that has gone away must not be retained again.
    const nickel_qt5::OpeningContext context{ data_->keys,
                                              data_->library ? data_->libraryController : nullptr,
                                              data_->bookmark };
    auto result = std::make_unique<NativeOpening>();
    return result->retain(data_->volume, &context) ? std::move(result) : nullptr;
}

const void *NativeOpening::volume() const
{
    return data_ ? data_->volume : nullptr;
}

const void *NativeOpening::bookmark() const
{
    return data_ ? data_->bookmark : nullptr;
}

const void *NativeOpening::keys() const
{
    return data_ ? data_->keys : nullptr;
}

void NativeOpening::openStock()
{
    if (!data_)
        return;
    // createReader ignores its mixin argument. Its native constructor accepts
    // a null library controller if the original QObject has since gone away.
    const auto &native = api();
    QPointer<QObject> opener = native.create(nullptr, data_->volume, data_->keys,
                                             data_->library ? data_->libraryController : nullptr);
    if (!opener)
        return;
    native.open(opener, data_->bookmark);
    if (opener && !opener->parent())
        opener->deleteLater();
}
} // namespace kobalt
