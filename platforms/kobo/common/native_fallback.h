#pragma once
#include <functional>
class QString;
class QObject;
class QDialog;

namespace kobalt {
// Nickel owns the modal styling and OK button; no bundled assets are needed.
QDialog *showMissingAssetsAlert(const QString &installedVersion, bool filesystemRecovery = false);

// Close the opening view before invoking Nickel, so its dialogs stay visible.
void bindReaderFallback(QObject *owner, QDialog *dialog, std::function<void()> openStock,
                        int automaticDelay = -1);
} // namespace kobalt
