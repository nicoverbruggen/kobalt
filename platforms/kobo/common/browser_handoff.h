#pragma once
#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QVariant>
#include <QWidget>
#include <functional>

namespace kobalt {
// Nickel puts its modal browser inside MainWindow. Hide the reader only when
// that browser appears, then restore it when the containing dialog closes.
class BrowserHandoff : public QObject
{
public:
    explicit BrowserHandoff(QObject *parent = nullptr) : QObject(parent)
    {
        qApp->installEventFilter(this);
    }

    static bool activeFor(const QWidget *reader)
    {
        return reader && reader->property("kobalt_native_browser").toBool();
    }

    void begin(QWidget *anchor, std::function<bool()> restore)
    {
        reader = anchor;
        canRestore = std::move(restore);
        waiting = true;
    }

    void check()
    {
        if (!waiting)
            return;
        for (auto *widget : QApplication::allWidgets()) {
            if (widget->isVisible() && widget->objectName() == "N3Dialog"
                && widget->findChild<QWidget *>("N3BrowserView")) {
                dialog = widget;
                waiting = false;
                hideReader();
                return;
            }
        }
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Close && object == reader) {
            reader->setProperty("kobalt_native_browser", false);
            reader.clear();
            canRestore = {};
            waiting = false;
        }
        auto *widget = waiting && event->type() == QEvent::Show ? qobject_cast<QWidget *>(object)
                                                                : nullptr;
        if (widget) {
            QWidget *browserDialog = nullptr;
            if (widget->objectName() == "N3Dialog" && widget->findChild<QWidget *>("N3BrowserView"))
                browserDialog = widget;
            else if (widget->objectName() == "N3BrowserView") {
                auto *parent = widget->parentWidget();
                while (parent && parent->objectName() != "N3Dialog")
                    parent = parent->parentWidget();
                browserDialog = parent;
            }
            if (browserDialog) {
                dialog = browserDialog;
                waiting = false;
                hideReader();
            }
        }
        if ((event->type() == QEvent::Hide || event->type() == QEvent::Destroy)
            && object == dialog) {
            // Sleep hides Nickel's whole page stack without closing its
            // browser. Retain the handoff until that dialog really closes.
            if (event->type() == QEvent::Hide && canRestore && !canRestore())
                return false;
            dialog.clear();
            if (reader)
                reader->setProperty("kobalt_native_browser", false);
            if (reader && canRestore && canRestore()) {
                reader->show();
                reader->raise();
                reader->activateWindow();
            }
            reader.clear();
            canRestore = {};
        }
        return false;
    }

private:
    void hideReader()
    {
        if (!reader)
            return;
        // The coordinator also restores readers hidden for sleep. Keep that
        // visibility check from raising the reader over the native browser.
        reader->setProperty("kobalt_native_browser", true);
        reader->hide();
    }

    QPointer<QWidget> reader, dialog;
    std::function<bool()> canRestore;
    bool waiting = false;
};
} // namespace kobalt
