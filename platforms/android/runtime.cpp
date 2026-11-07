#include "runtime.h"
#include <QGuiApplication>
#include <QInputMethod>
#include <QJniObject>
#include <QJniEnvironment>
#include <QPointer>
#include <atomic>
#include <QtCore/qnativeinterface.h>
#include <utility>

namespace {
QPointer<QObject> owner;
std::function<void(QString)> openExternal;
std::function<void(int, int)> volumeButton;
std::function<void(QString, QString)> chosenFolder;
std::function<void(QSize, QMargins, int)> resizedSurface;
std::atomic<bool> pageButtonsAvailable{ false };
} // namespace

namespace kobalt::android {
void setCallbacks(QObject *receiver, std::function<void(QString)> openBook,
                  std::function<void(int, int)> turnPage,
                  std::function<void(QString, QString)> folderChosen,
                  std::function<void(QSize, QMargins, int)> surfaceResized)
{
    pageButtonsAvailable.store(false);
    owner = receiver;
    openExternal = std::move(openBook);
    volumeButton = std::move(turnPage);
    chosenFolder = std::move(folderChosen);
    resizedSurface = std::move(surfaceResized);
}

void releaseFolder(const QString &uri)
{
    QNativeInterface::QAndroidApplication::context().callMethod<void>(
            "releaseFolder", QJniObject::fromString(uri).object<jstring>());
}

void openLibraryBook(const QString &uri, const QString &destination, qint64 size, qint64 modified)
{
    QNativeInterface::QAndroidApplication::context().callMethod<void>(
            "openLibraryBook", QJniObject::fromString(uri).object<jstring>(),
            QJniObject::fromString(destination).object<jstring>(), jlong(size), jlong(modified));
}

void setPageButtonsAvailable(bool available)
{
    pageButtonsAvailable.store(available);
}

void activity(const char *method)
{
    const QByteArray name(method);
    QNativeInterface::QAndroidApplication::runOnAndroidMainThread([name] {
        QNativeInterface::QAndroidApplication::context().callMethod<void>(name.constData());
    });
}

bool readingGeometryReady()
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jboolean>(
            "readerGeometryReady");
}

bool libraryGeometryReady()
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jboolean>(
            "libraryGeometryReady");
}

QSize libraryContentSize()
{
    const auto size = quint64(QNativeInterface::QAndroidApplication::context().callMethod<jlong>(
            "libraryContentSize"));
    return { int(size >> 32), int(size & 0xffffffff) };
}

int volumeButtons()
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jint>("volumeButtons");
}

int screenDensity()
{
    return qBound(
            100,
            int(QNativeInterface::QAndroidApplication::context().callMethod<jint>("readerDensity")),
            600);
}

void setLibraryDark(bool dark)
{
    QNativeInterface::QAndroidApplication::context().callMethod<void>("readerLibraryDark",
                                                                      jboolean(dark));
}

bool refreshAvailable()
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jboolean>(
            "readerRefreshAvailable");
}

bool prepareRefreshRegions(const QRegion &uiPixels, const QRegion &textPixels, bool clean)
{
    QVector<jint> entries;
    entries.reserve((uiPixels.rectCount() + textPixels.rectCount()) * 5);
    for (int text = 0; text < 2; ++text) {
        for (const auto &area : text ? textPixels : uiPixels) {
            entries << area.left() << area.top() << area.right() + 1 << area.bottom() + 1 << text;
        }
    }
    QJniEnvironment environment;
    const auto array = environment->NewIntArray(entries.size());
    if (!array) {
        environment.checkAndClearExceptions();
        return false;
    }
    environment->SetIntArrayRegion(array, 0, entries.size(), entries.constData());
    const bool accepted = QNativeInterface::QAndroidApplication::context().callMethod<jboolean>(
            "readerPrepareRefresh", array, jboolean(clean));
    environment->DeleteLocalRef(array);
    return accepted;
}

bool lightAvailable()
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jboolean>(
            "readerLightAvailable");
}

int lightValue(int operation, int type, int value)
{
    return QNativeInterface::QAndroidApplication::context().callMethod<jint>(
            "readerLight", operation, type, value);
}

Battery battery()
{
    const int value =
            QNativeInterface::QAndroidApplication::context().callMethod<jint>("readerBattery");
    return { value < 0 ? -1 : value % 256, value >= 256 };
}

void showKeyboard()
{
    QGuiApplication::inputMethod()->show();
}

void hideKeyboard()
{
    QGuiApplication::inputMethod()->hide();
}
} // namespace kobalt::android

extern "C" JNIEXPORT void JNICALL Java_org_kobalt_android_KobaltActivity_openBook(JNIEnv *env,
                                                                                  jobject,
                                                                                  jstring value)
{
    const auto text = env->GetStringUTFChars(value, nullptr);
    if (!text)
        return;
    const auto path = QString::fromUtf8(text);
    env->ReleaseStringUTFChars(value, text);
    QMetaObject::invokeMethod(qApp, [path] {
        if (owner && openExternal)
            openExternal(path);
    });
}

extern "C" JNIEXPORT void JNICALL Java_org_kobalt_android_KobaltActivity_surfaceResized(
        JNIEnv *, jobject, jint width, jint height, jint left, jint top, jint right, jint bottom,
        jint corner)
{
    const QSize pixels(width, height);
    if (pixels.isEmpty())
        return;
    const QMargins safe(left, top, right, bottom);
    QMetaObject::invokeMethod(qApp, [pixels, safe, corner] {
        if (owner && resizedSurface)
            resizedSurface(pixels, safe, corner);
    });
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_kobalt_android_KobaltActivity_turnPage(JNIEnv *, jobject, jint direction, jint volumeKey)
{
    // Android's UI thread must not wait for Qt while Qt can request Android UI work.
    if (!pageButtonsAvailable.load())
        return false;
    QMetaObject::invokeMethod(qApp, [direction, volumeKey] {
        if (owner && volumeButton)
            volumeButton(direction, volumeKey);
    });
    return true;
}

extern "C" JNIEXPORT void JNICALL Java_org_kobalt_android_KobaltActivity_folderChosen(JNIEnv *env,
                                                                                      jobject,
                                                                                      jstring uri,
                                                                                      jstring label)
{
    const auto text = [env](jstring value) {
        const auto chars = env->GetStringUTFChars(value, nullptr);
        if (!chars)
            return QString();
        const auto result = QString::fromUtf8(chars);
        env->ReleaseStringUTFChars(value, chars);
        return result;
    };
    const auto key = text(uri), name = text(label);
    if (key.isEmpty())
        return;
    QMetaObject::invokeMethod(qApp, [key, name] {
        if (owner && chosenFolder)
            chosenFolder(key, name);
    });
}
