#include "native_font_scale.h"
#include "support/qt_compat.h"
#include <QCoreApplication>
#include <QEvent>
#include <QHash>
#include <QPointer>
#include <QStyle>
#include <QVariant>
#include <QWidget>
#include <utility>

namespace kobalt::nickel {
namespace {
const char *const property = "fontScale";
const char *const holdName = "kobaltNativeFontScale";

bool kobaltWidget(QWidget *widget)
{
    return widget->window()->findChild<QObject *>(QLatin1String(holdName),
                                                  Qt::FindDirectChildrenOnly)
            || widget->property("reader_style").isValid();
}

// Widgets restyled under the other scale are styled again once the scale
// changes. Nickel keeps restyling its hidden Library while Kobalt is open;
// Kobalt can restyle its panels while a Nickel dialog has the scale. Which
// side a widget belongs to is decided only then, because a new Kobalt window
// gets its hold after its first polish.
class Restyled final : public QObject
{
public:
    explicit Restyled(bool kobalt) : kobalt(kobalt) { }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::FontChange && object->isWidgetType())
            widgets.insert(object, static_cast<QWidget *>(object));
        return false;
    }

    void take(Restyled &other)
    {
        for (auto it = other.widgets.cbegin(); it != other.widgets.cend(); ++it)
            widgets.insert(it.key(), it.value());
        other.widgets.clear();
    }

    void restyle()
    {
        const auto changed = std::exchange(widgets, {});
        for (const auto &widget : changed)
            if (widget && kobaltWidget(widget) == kobalt) {
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
            }
    }

private:
    bool kobalt;
    QHash<QObject *, QPointer<QWidget>> widgets;
};

int holders = 0, dialogs = 0;
QVariant nativeScale;
// Nickel widgets restyled while Kobalt kept the scale out, and Kobalt widgets
// restyled while a Nickel dialog had it.
Restyled *nickelRestyled = nullptr, *kobaltRestyled = nullptr;

void track(Restyled *&tracker, bool kobalt)
{
    if (tracker)
        return;
    tracker = new Restyled(kobalt);
    qApp->installEventFilter(tracker);
}

void finish(Restyled *&tracker)
{
    auto *done = std::exchange(tracker, nullptr);
    if (!done)
        return;
    qApp->removeEventFilter(done);
    // A hold can end while its window destroys its children. If the scale has
    // changed back by then, the next change restyles these widgets instead.
    auto *const active = &tracker;
    qt::later(0, done, [done, active] {
        if (*active)
            (*active)->take(*done);
        else
            done->restyle();
        done->deleteLater();
    });
}

// Kobalt keeps the scale out while one of its windows is open, except while a
// Nickel dialog shows over it.
void update()
{
    const bool cleared = !qApp->property(property).isValid();
    const bool clear = holders && !dialogs && nativeScale.isValid();
    if (clear && !cleared) {
        // An invalid value removes the dynamic property, as Nickel does at 100%.
        qApp->setProperty(property, QVariant());
        track(nickelRestyled, false);
        finish(kobaltRestyled);
    } else if (!clear && cleared && nativeScale.isValid()) {
        // Nickel sets the scale only at startup, during setup and from its
        // settings, none of which run over Kobalt. A newer value stays.
        qApp->setProperty(property, nativeScale);
        finish(nickelRestyled);
        if (holders)
            track(kobaltRestyled, true);
    }
}

void hold()
{
    if (holders++ == 0)
        nativeScale = qApp->property(property);
    update();
}

void release()
{
    --holders;
    update();
    if (!holders) {
        nativeScale = QVariant();
        finish(kobaltRestyled);
    }
}

class WindowHold final : public QObject
{
public:
    explicit WindowHold(QWidget *window) : QObject(window)
    {
        setObjectName(QLatin1String(holdName));
        window->installEventFilter(this);
        set(true);
    }

    ~WindowHold() override { set(false); }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == parent() && (event->type() == QEvent::Show || event->type() == QEvent::Hide))
            set(event->type() == QEvent::Show);
        return false;
    }

private:
    void set(bool next)
    {
        if (next == held)
            return;
        held = next;
        held ? hold() : release();
    }

    bool held = false;
};

const char *const dialogName = "kobaltNativeDialogScale";

// Gives a Nickel dialog Nickel's scale while it shows over Kobalt.
class DialogScale final : public QObject
{
public:
    explicit DialogScale(QWidget *dialog) : QObject(dialog), dialog(dialog)
    {
        setObjectName(QLatin1String(dialogName));
        dialog->installEventFilter(this);
    }

    ~DialogScale() override { set(false); }

    void set(bool next)
    {
        if (next == counted || (next && !holders))
            return;
        counted = next;
        dialogs += next ? 1 : -1;
        update();
    }

    // A dialog styled before it showed over Kobalt has the standard size.
    void restyle()
    {
        auto widgets = dialog->findChildren<QWidget *>();
        widgets.prepend(dialog);
        for (auto *widget : widgets) {
            widget->style()->unpolish(widget);
            widget->style()->polish(widget);
        }
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (object == dialog && event->type() == QEvent::Hide)
            set(false);
        return false;
    }

private:
    QWidget *dialog;
    bool counted = false;
};
} // namespace

NativeFontScaleScope::NativeFontScaleScope()
{
    hold();
}

NativeFontScaleScope::~NativeFontScaleScope()
{
    release();
}

void ignoreNativeFontScale(QWidget *window)
{
    if (window)
        new WindowHold(window);
}

void allowNativeFontScale(QWidget *dialog, bool polishing)
{
    if (!dialog
        || dialog->findChild<QObject *>(QLatin1String(holdName), Qt::FindDirectChildrenOnly))
        return;
    auto *scale = static_cast<DialogScale *>(
            dialog->findChild<QObject *>(QLatin1String(dialogName), Qt::FindDirectChildrenOnly));
    if (!scale)
        scale = new DialogScale(dialog);
    const bool restyle = !polishing && !qApp->property(property).isValid() && nativeScale.isValid();
    scale->set(true);
    if (restyle)
        scale->restyle();
}

qreal nativeFontScale()
{
    const auto value = holders ? nativeScale : qApp->property(property);
    return value.isValid() ? qMax<qreal>(1, value.toReal()) : 1;
}
} // namespace kobalt::nickel
