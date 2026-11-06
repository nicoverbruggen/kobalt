#include "support/qt_compat.h"
#include "platforms/kobo/common/native_keyboard.h"
#include "platform/api/kobo_refresh.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include <QApplication>
#include <QGraphicsEffect>
#include <QLineEdit>
#include <QLocale>
#include <QMetaType>
#include <QEvent>
#include <QTimer>
#include <QVariant>
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
    void (*frame)(void *, QWidget *) = symbol<decltype(frame)>("_ZN13KeyboardFrameC1EP7QWidget");
    void (*destroyFrame)(QWidget *) = symbol<decltype(destroyFrame)>("_ZN13KeyboardFrameD0Ev");
    QObject *(*create)(QWidget *, int, const QLocale &) = symbol<decltype(create)>(
            "_ZN13KeyboardFrame14createKeyboardE14KeyboardScriptRK7QLocale");
    void (*receiver)(void *, QLineEdit *,
                     bool) = symbol<decltype(receiver)>("_ZN16KeyboardReceiverC1EP9QLineEditb");
    void (*attach)(QObject *, QObject *, bool) = symbol<decltype(attach)>(
            "_ZN24SearchKeyboardController11setReceiverEP16KeyboardReceiverb");
    void (*goText)(QObject *, const QString &) =
            symbol<decltype(goText)>("_ZN24SearchKeyboardController9setGoTextERK7QString");
    QMetaType frameType{ symbol<const QtPrivate::QMetaTypeInterface *>(
            "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI13KeyboardFrameE8metaTypeE") };
    QMetaType receiverType{ symbol<const QtPrivate::QMetaTypeInterface *>(
            "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI16KeyboardReceiverE8metaTypeE") };

    bool valid() const
    {
        return frame && destroyFrame && create && receiver && attach && goText
                && frameType.sizeOf() == 40 && receiverType.sizeOf() == 36;
    }
};

const Api &api()
{
    static const Api value;
    return value;
}
} // namespace

NativeKeyboard::NativeKeyboard(QWidget *parent, QLineEdit *editor)
    : QObject(parent),
      editor(editor),
      host(parent),
      window(parent->window()),
      submitText(strings::get("search_panel.search"))
{
    editor->installEventFilter(this);
}

NativeKeyboard::~NativeKeyboard()
{
    clearBounds();
    if (frame)
        api().destroyFrame(frame);
}

bool NativeKeyboard::create()
{
    if (!api().valid() || !editor || !host)
        return false;
    if (!frame) {
        // 5.18 exports both allocation sizes in its Qt metatypes. The native
        // deleting destructor releases the frame and its QObject children.
        void *storage = ::operator new(size_t(api().frameType.sizeOf()));
        try {
            api().frame(storage, host);
        } catch (...) {
            ::operator delete(storage);
            return false;
        }
        frame = static_cast<QWidget *>(storage);
        frame->setObjectName("rrSearchKeyboard");
        frame->installEventFilter(this);
        frame->setPalette(QApplication::palette());
        frame->setStyleSheet("QWidget{background:white;color:black;}");
        styleNativeKeyboard(frame);
    }
    auto *controller = api().create(frame, script, QLocale());
    if (!controller)
        return false;
    // AbstractController reparents itself to the VirtualKeyboard view when
    // loading it. That view remains a child of our KeyboardFrame.
    auto *owner = qobject_cast<QWidget *>(controller->parent());
    if (!owner || (owner != frame && !frame->isAncestorOf(owner)))
        return false;
    void *storage = ::operator new(size_t(api().receiverType.sizeOf()));
    try {
        api().receiver(storage, editor, false);
    } catch (...) {
        ::operator delete(storage);
        return false;
    }
    // LibraryFinderController uses this same sequence. The receiver constructor
    // parents it to the editor; setReceiver retains a guarded QObject pointer.
    api().attach(controller, static_cast<QObject *>(storage), true);
    const auto caption = editor->property("reader_keyboard_submit_text").toString();
    api().goText(controller, caption.isEmpty() ? submitText : caption);
    connect(controller, SIGNAL(commitRequested()), this, SLOT(commit()));
    connect(controller, SIGNAL(returnPressed()), this, SLOT(commit()));
    connect(controller, SIGNAL(languageChanged(KeyboardScript)), this,
            SLOT(changeScript(KeyboardScript)));
    for (auto *widget : frame->findChildren<QWidget *>())
        platform::textRefresh(widget);
    platform::textRefresh(frame);
    ready = true;
    return true;
}

bool NativeKeyboard::show()
{
    if (!host || !editor)
        return false;
    if (!ready && !create()) {
        diagnostic("search-keyboard-unavailable");
        return false;
    }
    QPointer<NativeKeyboard> guard(this);
    const bool opening = !frame->isVisible();
    place();
    if (!guard || !frame || !host || !editor)
        return false;
    if (opening) {
        frame->show();
        if (!guard || !frame)
            return false;
        frame->raise();
    }
    publishBounds();
    if (!guard || !frame || !editor)
        return false;
    // Redraw the theme only when opening, not for queued focus events.
    if (opening && frame->graphicsEffect())
        frame->graphicsEffect()->update();
    editor->setFocus();
    return bool(guard);
}

void NativeKeyboard::place()
{
    if (!frame || !host)
        return;
    frame->resize(host->width(), qMax(1, frame->sizeHint().height()));
    const int height = qMax(1, frame->sizeHint().height());
    frame->setGeometry(0, host->height() - height, host->width(), height);
    if (frame->isVisible())
        publishBounds();
}

void NativeKeyboard::hide()
{
    const auto visibleFrame = frame;
    clearBounds();
    if (visibleFrame)
        visibleFrame->hide();
}

void NativeKeyboard::commit()
{
    // Search hides the keyboard after submission. Ignore the follow-up signal.
    if (frame && frame->isVisible() && host && host->isVisible()) {
        const auto callback = submitted;
        if (callback)
            callback();
    }
}

void NativeKeyboard::changeScript(KeyboardScript value)
{
    script = int(value);
    ready = false;
    kobalt::qt::later(0, this, [this] {
        // Dismissing the editor cancels this opening, but keeps its chosen script.
        if (host && host->isVisible() && frame && frame->isVisible())
            show();
    });
}

bool NativeKeyboard::eventFilter(QObject *object, QEvent *event)
{
    QPointer<NativeKeyboard> guard(this);
    if (object == frame && event->type() == QEvent::Hide)
        clearBounds();
    if (!guard)
        return false;
    if (object == editor
        && (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonRelease))
        kobalt::qt::later(0, this, [this] {
            if (host && host->isVisible() && editor && editor->hasFocus())
                show();
        });
    return QObject::eventFilter(object, event);
}
} // namespace kobalt
