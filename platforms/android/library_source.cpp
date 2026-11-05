#include "library_source.h"
#include <QCoreApplication>
#include <QFile>
#include <QJniEnvironment>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#include <unistd.h>

namespace kobalt::android {
bool LibrarySource::list(const library::Folder &folder, QVector<library::Entry> &files,
                         const std::atomic<bool> &cancelled)
{
    // Qt attaches this scan thread to the Java VM for the call.
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    const auto result =
            context.callObjectMethod("listLibraryFolder", "(Ljava/lang/String;)[Ljava/lang/String;",
                                     QJniObject::fromString(folder.key).object<jstring>());
    if (!result.isValid())
        return false;
    QJniEnvironment env;
    const auto array = result.object<jobjectArray>();
    const int count = env->GetArrayLength(array);
    const auto item = [&env, array](int index) {
        auto *value = static_cast<jstring>(env->GetObjectArrayElement(array, index));
        const auto text = QJniObject(value).toString();
        env->DeleteLocalRef(value);
        return text;
    };
    for (int i = 0; i + 3 < count && !cancelled.load(); i += 4)
        files.append({ item(i), folder.key, item(i + 1), item(i + 2).toLongLong(),
                       item(i + 3).toLongLong() });
    return !cancelled.load();
}

std::unique_ptr<QIODevice> LibrarySource::open(const library::Entry &entry)
{
    const int descriptor = QNativeInterface::QAndroidApplication::context().callMethod<jint>(
            "openLibraryDocument", QJniObject::fromString(entry.key).object<jstring>());
    if (descriptor < 0)
        return {};
    auto file = std::make_unique<QFile>();
    // A provider may hand out a pipe; the metadata reader needs to seek.
    if (!file->open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)
        || file->isSequential()) {
        if (!file->isOpen())
            ::close(descriptor);
        return {};
    }
    return file;
}
} // namespace kobalt::android
