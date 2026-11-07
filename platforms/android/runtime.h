#pragma once
#include <QObject>
#include <QString>
#include <QRegion>
#include <QSize>
#include <QMargins>
#include <functional>

namespace kobalt::android {
// Install on Qt's GUI thread. Delivery stops when owner is destroyed.
void setCallbacks(QObject *owner, std::function<void(QString)> openBook,
                  std::function<void(int, int)> turnPage,
                  std::function<void(QString, QString)> folderChosen = {},
                  std::function<void(QSize, QMargins, int corner)> surfaceResized = {});
void setPageButtonsAvailable(bool available);
void setLibraryDark(bool dark);
void activity(const char *method);
bool readingGeometryReady();
bool libraryGeometryReady();
QSize libraryContentSize();
int screenDensity();
int volumeButtons();
bool refreshAvailable();
bool prepareRefreshRegions(const QRegion &uiPixels, const QRegion &textPixels, bool clean);
bool lightAvailable();
int lightValue(int operation, int type, int value = 0);

struct Battery
{
    int percent = -1;
    bool charging = false;
};

Battery battery();
void showKeyboard();
// Library folders are granted document trees, read through the activity.
void releaseFolder(const QString &uri);
void openLibraryBook(const QString &uri, const QString &destination, qint64 size, qint64 modified);
void hideKeyboard();
} // namespace kobalt::android
