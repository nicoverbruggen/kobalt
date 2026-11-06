#include "keyboard.h"
#include "buttons.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/theme.h"
#include "ui/style/scale.h"
#include "platform/api/kobo_refresh.h"
#include "reading/reading_density.h"
#include "support/strings.h"
#include "ui/input/hold_timer.h"
#include "ui/input/input.h"
#include <QEvent>
#include <QApplication>
#include <QGridLayout>
#include <QHash>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QWidget>
#include <array>

namespace kobalt::ui {
namespace {
class ShiftButton final : public Button
{
public:
    using Button::Button;
    quint32 releasedAt = 0;
    bool pointerRelease = false;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        // Compare source times even when e-ink painting delays both taps.
        QPointer<ShiftButton> guard(this);
        releasedAt = quint32(event->timestamp());
        pointerRelease = event->timestamp() != 0;
        Button::mouseReleaseEvent(event);
        if (guard)
            pointerRelease = false;
    }
};

QStringList alphabetRows()
{
    switch (QLocale().language()) {
    case QLocale::German:
        return { "", "qwertzuiop", "asdfghjkl", "yxcvbnm" };
    case QLocale::French:
        if (QLocale().name() != QStringLiteral("fr_CA"))
            return { "", "azertyuiop", "qsdfghjklm", "wxcvbn" };
        break;
    default:
        break;
    }
    return { "", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
}

void characterFont(QPushButton *key)
{
    readerStyle(
            key,
            QStringLiteral(
                    "QPushButton{font-family:\"@ui-font-family@\";font-size:%1px;border-radius:%2px;}")
                    .arg(px(48))
                    .arg(px(12)));
}

QString accents(const QString &letter)
{
    static const QHash<QString, QString> variants{
        { "a", QString::fromUtf8("àáâãäåāăąæ") }, { "c", QString::fromUtf8("çćčĉċ") },
        { "d", QString::fromUtf8("ďđð") },        { "e", QString::fromUtf8("èéêëēĕėęě") },
        { "g", QString::fromUtf8("ğģĝġ") },       { "h", QString::fromUtf8("ħĥ") },
        { "i", QString::fromUtf8("ìíîïīĭįı") },   { "j", QString::fromUtf8("ĵ") },
        { "k", QString::fromUtf8("ķ") },          { "l", QString::fromUtf8("ĺļľł") },
        { "n", QString::fromUtf8("ñńņňŋ") },      { "o", QString::fromUtf8("òóôõöøōŏőœ") },
        { "r", QString::fromUtf8("ŕŗř") },        { "s", QString::fromUtf8("ßśŝşšș") },
        { "t", QString::fromUtf8("ţťŧțþ") },      { "u", QString::fromUtf8("ùúûüūŭůűų") },
        { "w", QString::fromUtf8("ŵ") },          { "y", QString::fromUtf8("ýÿŷ") },
        { "z", QString::fromUtf8("źżž") }
    };
    auto result = variants.value(letter.toLower());
    if (letter != letter.toLower()) {
        result.replace(QString::fromUtf8("ß"), QString::fromUtf8("ẞ"));
        result = result.toUpper();
        if (letter == "I")
            result += QString::fromUtf8("İ");
    }
    return result;
}

class WidgetKeyboard final : public QObject, public KeyboardController
{
public:
    WidgetKeyboard(QWidget *parent, QLineEdit *editor)
        : editor(editor), panel(new QWidget(parent)), window(panel->window())
    {
        panel->setObjectName("widgetKeyboard");
        panel->setAutoFillBackground(true);
        // Gaps and cancelled key releases must not reach a modal backdrop.
        panel->setAttribute(Qt::WA_NoMousePropagation);
        readerStyle(
                panel,
                QStringLiteral(
                        "QWidget#widgetKeyboard{background:palette(window);border-top:2px solid palette(window-text);}"
                        "QWidget#widgetKeyboard QPushButton{border:%1px solid palette(mid);border-radius:%2px;background:palette(window);"
                        "color:palette(window-text);padding:%3px;}"
                        "QWidget#widgetKeyboard QPushButton:pressed,QWidget#widgetKeyboard QPushButton:checked{"
                        "background:palette(window-text);color:palette(window);}"
                        "QWidget#widgetKeyboard QPushButton#widgetKeyboardDone{background:palette(window-text);color:palette(window);border-color:palette(window-text);}"
                        "QWidget#widgetKeyboard QPushButton#widgetKeyboardDone:pressed{background:palette(window);color:palette(window-text);}")
                        .arg(int(px(2)))
                        .arg(int(px(12)))
                        .arg(int(px(8))));
        auto *layout = new QVBoxLayout(panel);
        for (int row = 0; row < letterRows.size(); ++row) {
            rowWidgets[row] = new QWidget(panel);
            auto *line = new QHBoxLayout(rowWidgets[row]);
            line->setContentsMargins(0, 0, 0, 0);
            line->addStretch();
            if (row == letterRows.size() - 1)
                lastRow = line;
            layout->addWidget(rowWidgets[row], 1);
            for (int column = 0; column < qMax(letterRows[row].size(), symbolRows[row].size());
                 ++column) {
                auto *key = new Button(panel);
                key->setObjectName(QStringLiteral("widgetKey%1_%2").arg(row).arg(column));
                key->setFocusPolicy(Qt::NoFocus);
                key->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
                key->installEventFilter(this);
                platform::textRefresh(key);
                line->addWidget(key, 1);
                keys[row].append(key);
                QObject::connect(key, &QPushButton::clicked, this, [this, key] {
                    closeAccents();
                    insert(key->property("keyboard.character").toString(), !symbolPage);
                });
            }
            line->addStretch();
        }
        auto *actions = new QHBoxLayout;
        layout->addLayout(actions);
        shift = new ShiftButton(panel);
        symbols = new Button(panel);
        symbols->setAccessibleName(strings::get("keyboard.symbols"));
        shift->setObjectName("widgetKeyboardShift");
        symbols->setObjectName("widgetKeyboardSymbols");
        shift->setCheckable(true);
        symbols->setCheckable(true);
        QObject::connect(shift, &QPushButton::clicked, this, [this] {
            const auto interval = quint32(QApplication::doubleClickInterval());
            const bool doubleTap = shift->pointerRelease && pointerShift
                    ? quint32(shift->releasedAt - shiftedAt) <= interval
                    : shiftTap.isValid() && shiftTap.elapsed() <= interval;
            if (caseMode == Case::Lower) {
                caseMode = Case::Once;
                shiftedAt = shift->releasedAt;
                pointerShift = shift->pointerRelease;
                shiftTap.start();
            } else {
                caseMode = caseMode == Case::Once && doubleTap ? Case::Locked : Case::Lower;
                endShiftTap();
            }
            render();
        });
        QObject::connect(symbols, &QPushButton::toggled, this, [this](bool enabled) {
            endShiftTap();
            symbolPage = enabled;
            render();
        });
        auto *space = new Button(strings::get("keyboard.space"), panel);
        erase = new Button(panel);
        done = new Button(strings::get("keyboard.done"), panel);
        for (const auto &item :
             { qMakePair(static_cast<Button *>(shift), ControlIcon::KeyboardShift),
               qMakePair(erase, ControlIcon::KeyboardBackspace),
               qMakePair(done.data(), ControlIcon::KeyboardReturn) }) {
            buttonIcon(item.first, item.second);
            item.first->setIconSize(QSize(px(48), px(48)));
            item.first->setIconLabelGap(px(8));
        }
        shift->setAccessibleName(strings::get("keyboard.shift"));
        erase->setAccessibleName(strings::get("keyboard.backspace"));
        done->setAccessibleName(strings::get("keyboard.done"));
        space->setObjectName("widgetKeyboardSpace");
        erase->setObjectName("widgetKeyboardDelete");
        // Keyboard keys invert their background and icon while pressed.
        erase->setProperty("reader_invert_pressed", true);
        done->setObjectName("widgetKeyboardDone");
        done->setProperty("reader_primary_action", true);
        for (auto *key :
             std::array<QPushButton *, 5>{ shift, symbols, space, erase, done.data() }) {
            key->installEventFilter(this);
            key->setFocusPolicy(Qt::NoFocus);
            key->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            platform::textRefresh(key);
        }
        lastRow->insertWidget(1, shift);
        lastRow->insertWidget(lastRow->count() - 1, erase);
        actions->addWidget(symbols, 1);
        actions->addWidget(space, 4);
        actions->addWidget(done, 2);
        layout->setStretch(layout->count() - 1, 1);
        render();
        QObject::connect(space, &QPushButton::clicked, this, [this] {
            closeAccents();
            insert(QStringLiteral(" "), true);
        });
        QObject::connect(erase, &QPushButton::clicked, this, [this] {
            closeAccents();
            endShiftTap();
            if (this->editor)
                this->editor->backspace();
        });
        QObject::connect(done, &QPushButton::clicked, this, [this] {
            const auto callback = submitted;
            QPointer<WidgetKeyboard> guard(this);
            hide();
            if (guard && callback)
                callback();
        });
        panel->hide();
        hold.setSingleShot(true);
        hold.setInterval(metrics::input::longPressMilliseconds);
        QObject::connect(&hold, &QTimer::timeout, this, [this] { showAccents(); });
        platform::textRefresh(panel);
        panel->installEventFilter(this);
        if (editor)
            editor->installEventFilter(this);
        QObject::connect(panel, &QObject::destroyed, this,
                         [this, owner = panel.data()] { clearBounds(owner); });
    }

    ~WidgetKeyboard() override
    {
        hide();
        // A bounds callback can destroy the controller inside QWidget::show().
        // Keep the panel alive until Qt has finished dispatching that event.
        if (panel)
            panel->deleteLater();
    }

    bool show() override
    {
        if (!editor || !panel)
            return false;
        QPointer<WidgetKeyboard> guard(this);
        editor->setFocus();
        if (!guard || !panel)
            return false;
        place();
        if (!guard || !panel)
            return false;
        if (!panel->isVisible()) {
            panel->show();
            if (!guard || !panel)
                return false;
            panel->raise();
        }
        return true;
    }

    void setSubmitText(const QString &text) override
    {
        if (done) {
            done->setText(text);
            done->setAccessibleName(text);
        }
    }

    void hide() override
    {
        closeAccents();
        const auto visiblePanel = panel;
        clearBounds(panel);
        if (visiblePanel)
            visiblePanel->hide();
    }

    void place() override
    {
        if (!panel || !panel->parentWidget())
            return;
        const auto bounds = panel->parentWidget()->rect();
        // A third of the screen, but at least four rows of keys as tall as
        // their 48-pixel labels with padding and border, about 80 pixels, and
        // the gaps between them. Shorter keys clip their labels and grow back
        // when restyled; this matters on a short screen.
        const auto margins = panel->layout()->contentsMargins();
        const int keyRows =
                4 * px(80) + 3 * panel->layout()->spacing() + margins.top() + margins.bottom();
        const int height = qMin(bounds.height() / 2, qMax(qMax(220, bounds.height() / 3), keyRows));
        const bool resized = panel->size() != QSize(bounds.width(), height);
        if (resized)
            closeAccents();
        panel->setGeometry(0, bounds.height() - height, bounds.width(), height);
        if (resized)
            render();
    }

private:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        QPointer<WidgetKeyboard> guard(this);
        if (event->type() == QEvent::TouchCancel) {
            closeAccents();
            if (auto *key = qobject_cast<QPushButton *>(object))
                key->setDown(false);
        }
        if (object == editor && event->type() == QEvent::KeyPress && popup
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            closeAccents();
            return true;
        }
        auto *key = qobject_cast<QPushButton *>(object);
        if (key && key->property("keyboard.character").isValid()) {
            if (event->type() == QEvent::MouseButtonPress
                && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                closeAccents();
                pressedKey = key;
                press = mousePosition(static_cast<QMouseEvent *>(event)).toPoint();
                if (!accents(key->property("keyboard.character").toString()).isEmpty())
                    hold.start(static_cast<QMouseEvent *>(event));
            } else if (event->type() == QEvent::MouseMove && pressedKey == key && !popup) {
                if ((mousePosition(static_cast<QMouseEvent *>(event)).toPoint() - press)
                            .manhattanLength()
                    > px(12))
                    hold.stop();
            } else if (event->type() == QEvent::MouseButtonRelease && pressedKey == key) {
                if (hold.finish(static_cast<QMouseEvent *>(event)))
                    showAccents();
                pressedKey = nullptr;
                if (popup) {
                    key->setDown(false);
                    const auto position = popup->mapFromGlobal(key->mapToGlobal(
                            mousePosition(static_cast<QMouseEvent *>(event)).toPoint()));
                    auto *choice = qobject_cast<QPushButton *>(popup->childAt(position));
                    if (choice
                        && (mousePosition(static_cast<QMouseEvent *>(event)).toPoint() - press)
                                        .manhattanLength()
                                > px(12))
                        choice->click();
                    return true;
                }
            }
        }
        if (object == editor && event->type() == QEvent::MouseButtonRelease
            && editor->isVisible()) {
            show();
            if (!guard)
                return true;
        }
        if (object == editor && panel && panel->isVisible() && window
            && (event->type() == QEvent::Move || event->type() == QEvent::Resize))
            window->setProperty("reader_keyboard_editor_bounds",
                                QRect(editor->mapTo(window, QPoint()), editor->size()));
        if (object == panel) {
            if (event->type() == QEvent::Hide) {
                closeAccents();
                clearBounds(panel);
            } else if (event->type() == QEvent::MouseButtonPress) {
                closeAccents();
            } else if (window && panel->isVisible()
                       && (event->type() == QEvent::Show || event->type() == QEvent::Resize
                           || event->type() == QEvent::Move)) {
                const auto host = window;
                const auto owner = panel;
                const QRect bounds(panel->mapTo(host, QPoint()), panel->size());
                host->setProperty("reader_keyboard_editor_bounds",
                                  editor ? QRect(editor->mapTo(host, QPoint()), editor->size())
                                         : QRect());
                host->setProperty("reader_keyboard_widget", QVariant::fromValue<QObject *>(owner));
                if (host && owner
                    && host->property("reader_keyboard_widget").value<QObject *>() == owner)
                    host->setProperty("reader_keyboard_bounds", bounds);
            }
        }
        return !guard || QObject::eventFilter(object, event);
    }

    void closeAccents()
    {
        hold.stop();
        if (pressedKey)
            pressedKey->setDown(false);
        pressedKey = nullptr;
        if (popup) {
            popup->hide();
            popup->setObjectName(QString());
            popup->deleteLater();
            popup = nullptr;
        }
    }

    void showAccents()
    {
        if (!panel || !panel->isVisible() || !pressedKey || !pressedKey->isDown() || popup)
            return;
        const auto choices = accents(pressedKey->property("keyboard.character").toString());
        if (choices.isEmpty())
            return;
        popup = new QWidget(panel);
        popup->setObjectName("widgetKeyboardAccents");
        readerStyle(
                popup,
                "QWidget#widgetKeyboardAccents{background:palette(window);border:2px solid palette(window-text);}");
        auto *grid = new QGridLayout(popup);
        grid->setContentsMargins(4, 4, 4, 4);
        grid->setSpacing(4);
        const int count = int(choices.size());
        const int columns = qMin(6, count);
        const int rows = (count + columns - 1) / columns;
        for (int index = 0; index < choices.size(); ++index) {
            const auto value = choices.mid(index, 1);
            auto *choice = new Button(value, popup);
            choice->setObjectName(QStringLiteral("widgetAccent%1")
                                          .arg(uint(choices[index].unicode()), 4, 16, QChar('0')));
            choice->setProperty("keyboard.accent", value);
            choice->setFocusPolicy(Qt::NoFocus);
            characterFont(choice);
            choice->installEventFilter(this);
            choice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
            platform::textRefresh(choice);
            grid->addWidget(choice, index / columns, index % columns);
            QObject::connect(choice, &QPushButton::clicked, this, [this, value] {
                closeAccents();
                insert(value, true);
            });
        }
        const int width =
                qMin(panel->width(),
                     qMax(columns * pressedKey->width(), popup->minimumSizeHint().width()));
        const int height =
                qMin(panel->height(),
                     qMax(rows * pressedKey->height(), popup->minimumSizeHint().height()));
        const auto anchor = pressedKey->mapTo(panel, QPoint());
        const int x =
                qBound(0, anchor.x() + pressedKey->width() / 2 - width / 2, panel->width() - width);
        const int y = qBound(0, anchor.y() - height, panel->height() - height);
        popup->setGeometry(x, y, width, height);
        platform::textRefresh(popup);
        popup->show();
        popup->raise();
    }

    void clearBounds(QObject *owner)
    {
        const auto host = window;
        if (host && owner && host->property("reader_keyboard_widget").value<QObject *>() == owner) {
            host->setProperty("reader_keyboard_editor_bounds", QRect());
            host->setProperty("reader_keyboard_widget", QVariant());
            if (host && !host->property("reader_keyboard_widget").value<QObject *>())
                host->setProperty("reader_keyboard_bounds", QRect());
        }
    }

    const QStringList letterRows = alphabetRows();
    const QStringList symbolRows{ "1234567890", "!@#$%^&*()", "-_=+[]{}\\|", ";:'\",.<>/?`~" };
    std::array<QVector<QPushButton *>, 4> keys;
    std::array<QWidget *, 4> rowWidgets;
    ShiftButton *shift = nullptr;
    Button *erase = nullptr;
    QPushButton *symbols = nullptr;
    QHBoxLayout *lastRow = nullptr;
    enum class Case { Lower, Once, Locked };
    Case caseMode = Case::Lower;
    bool symbolPage = false;
    QElapsedTimer shiftTap;
    quint32 shiftedAt = 0;
    bool pointerShift = false;

    void endShiftTap()
    {
        shiftTap.invalidate();
        pointerShift = false;
    }

    void insert(const QString &text, bool consumeShift)
    {
        endShiftTap();
        if (!editor)
            return;
        QPointer<WidgetKeyboard> guard(this);
        const auto before = editor->text();
        const bool selected = editor->hasSelectedText();
        editor->insert(text);
        if (!guard)
            return;
        if (editor && consumeShift && caseMode == Case::Once
            && (editor->text() != before || (selected && !editor->hasSelectedText()))) {
            caseMode = Case::Lower;
            render();
        }
    }

    void render()
    {
        closeAccents();
        shift->setChecked(caseMode != Case::Lower);
        buttonIcon(shift,
                   caseMode == Case::Locked ? ControlIcon::KeyboardCapsLock
                                            : ControlIcon::KeyboardShift);
        shift->setAccessibleName(
                strings::get(caseMode == Case::Locked ? "keyboard.caps_lock" : "keyboard.shift"));
        const auto &rows = symbolPage ? symbolRows : letterRows;
        symbols->setText(symbolPage ? QStringLiteral("ABC") : QStringLiteral("123"));
        shift->setVisible(!symbolPage);
        const int spacing = lastRow->spacing();
        const auto margins = panel->layout()->contentsMargins();
        const auto widthFor = [&](int columns) {
            return qMax(
                    1,
                    (panel->width() - margins.left() - margins.right() - (columns - 1) * spacing)
                            / columns);
        };
        const int width = widthFor(symbolPage ? 14 : 10);
        // A narrow screen gives the symbol rows of ten the width of letter
        // keys. Only the longer punctuation row keeps the narrow keys.
        const bool narrow = isNarrowWidth(panel->width(), density());
        const int rowCount = symbolPage ? 5 : 4;
        const int height = qMax(1,
                                (panel->height() - margins.top() - margins.bottom()
                                 - (rowCount - 1) * panel->layout()->spacing())
                                        / rowCount);
        shift->setFixedWidth(width * 3 / 2);
        erase->setFixedWidth(width * 3 / 2);
        shift->setFixedHeight(height);
        erase->setFixedHeight(height);
        // Style the keys before showing their row. Showing polishes them, and
        // a style sheet on a polished widget polishes it again.
        for (int row = 0; row < rows.size(); ++row) {
            for (int column = 0; column < keys[row].size(); ++column) {
                auto *key = keys[row][column];
                const auto character =
                        column < rows[row].size() ? rows[row].mid(column, 1) : QString();
                const auto shown =
                        caseMode != Case::Lower && !symbolPage ? character.toUpper() : character;
                key->setProperty("keyboard.character", shown);
                key->setText(shown == "&" ? QStringLiteral("&&") : shown);
                characterFont(key);
                key->setVisible(!shown.isEmpty());
                key->setFixedSize(narrow && rows[row].size() <= 10 ? widthFor(10) : width, height);
            }
            rowWidgets[row]->setVisible(!rows[row].isEmpty());
        }
    }

    QPointer<Button> done;
    QPointer<QLineEdit> editor;
    QPointer<QWidget> panel;
    QPointer<QWidget> window;
    QPointer<QWidget> popup;
    QPointer<QPushButton> pressedKey;
    HoldTimer hold;
    QPoint press;
};
} // namespace

std::unique_ptr<KeyboardController> makeWidgetKeyboard(QWidget *parent, QLineEdit *editor)
{
    return std::make_unique<WidgetKeyboard>(parent, editor);
}
} // namespace kobalt::ui
