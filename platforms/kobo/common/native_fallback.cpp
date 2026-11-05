#include "native_fallback.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include <QApplication>
#include <QThread>
#include <QDialog>
#include <dlfcn.h>
#include <memory>
#include <utility>

namespace kobalt {
QDialog *showMissingAssetsAlert(const QString &installedVersion, bool filesystemRecovery)
{
    if (!qApp || QThread::currentThread() != qApp->thread())
        return nullptr;
    using ShowError = QDialog *(*)(const QString &, const QString &);
    static const auto show = reinterpret_cast<ShowError>(
            dlsym(RTLD_DEFAULT, "_ZN25ConfirmationDialogFactory15showErrorDialogERK7QStringS2_"));
    return show ? show(strings::get("native_ui.missing_assets_title"),
                       strings::get(filesystemRecovery ? "native_ui.recovered_assets_message"
                                                       : "native_ui.missing_assets_message")
                               .arg(installedVersion))
                : nullptr;
}

void bindReaderFallback(QObject *owner, QDialog *dialog, std::function<void()> openStock,
                        int automaticDelay)
{
    // Qt 5.2 copies the functor when invoking it. Keep the consumed state shared
    // so acceptance runs once and cancellation releases the retained request.
    const auto pending = std::make_shared<std::function<void()>>(std::move(openStock));
    QObject::connect(dialog, &QDialog::finished, owner, [pending](int result) {
        auto action = std::exchange(*pending, {});
        if (result == QDialog::Accepted && action)
            action();
    });
    QObject::connect(owner, &QObject::destroyed, dialog, &QDialog::reject);
    if (automaticDelay >= 0)
        qt::later(automaticDelay, dialog, [dialog, pending] {
            if (*pending && dialog->isVisible())
                dialog->accept();
        });
}
} // namespace kobalt
