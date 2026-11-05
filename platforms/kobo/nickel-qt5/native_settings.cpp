#include "native_settings.h"
#include <QApplication>
#include <QEvent>
#include <QWidget>
#include <utility>

namespace kobalt::nickel_qt5 {
namespace {
class SettingsPages : public QObject
{
public:
    SettingsPages(QObject *owner, std::function<void(QWidget *)> install)
        : QObject(owner), install(std::move(install))
    {
    }

    void inspect(QWidget *view)
    {
        if (view->inherits("N3SettingsReadingView"))
            install(view);
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if ((event->type() == QEvent::Polish || event->type() == QEvent::Show)
            && object->isWidgetType())
            inspect(static_cast<QWidget *>(object));
        return false;
    }

private:
    std::function<void(QWidget *)> install;
};
} // namespace

void watchReadingSettings(QObject *owner, std::function<void(QWidget *)> install)
{
    auto *pages = new SettingsPages(owner, std::move(install));
    qApp->installEventFilter(pages);
    for (auto *view : QApplication::allWidgets())
        pages->inspect(view);
}
} // namespace kobalt::nickel_qt5
