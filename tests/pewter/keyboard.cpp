#include "platforms/kobo/common/native_keyboard.h"
#include <QApplication>
#include <QLineEdit>
#include <QMetaType>
#include <QVariant>
#include <cstdio>
#include <cstdlib>

enum KeyboardScript : int { Latin, Symbols };

// These synthetic widgets model the adapter's ownership contract, not Nickel's layout.
class KeyboardReceiver final : public QObject
{
    Q_OBJECT
public:
    KeyboardReceiver(QLineEdit *editor, bool password);
    char padding[36 - sizeof(QObject)];
};

KeyboardReceiver::KeyboardReceiver(QLineEdit *editor, bool password) : QObject(editor)
{
    setProperty("password", password);
}

class SearchKeyboardController final : public QObject
{
    Q_OBJECT
public:
    explicit SearchKeyboardController(QWidget *view) : QObject(view) { }

    void setReceiver(KeyboardReceiver *value, bool owned);
    void setGoText(const QString &text);
    QPointer<KeyboardReceiver> receiver;
signals:
    void commitRequested();
    void returnPressed();
    void languageChanged(KeyboardScript script);
};

void SearchKeyboardController::setReceiver(KeyboardReceiver *value, bool owned)
{
    receiver = value;
    setProperty("owned", owned);
}

void SearchKeyboardController::setGoText(const QString &text)
{
    setProperty("caption", text);
}

class KeyboardFrame final : public QWidget
{
    Q_OBJECT
public:
    explicit KeyboardFrame(QWidget *parent);
    ~KeyboardFrame() override;
    QObject *createKeyboard(KeyboardScript script, const QLocale &locale);

    QSize sizeHint() const override { return QSize(600, 240); }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    char padding[40 - sizeof(QWidget)];
#else
    char padding[28 - sizeof(QWidget)];
#endif
};

KeyboardFrame::KeyboardFrame(QWidget *parent) : QWidget(parent) { }

KeyboardFrame::~KeyboardFrame() = default;

QObject *KeyboardFrame::createKeyboard(KeyboardScript script, const QLocale &)
{
    if (auto *previous = findChild<QWidget *>("fakeKeyboardView"))
        delete previous;
    setProperty("script", int(script));
    auto *view = new QWidget(this);
    view->setObjectName("fakeKeyboardView");
    return new SearchKeyboardController(view);
}

static void require(bool value, const char *message)
{
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

// The production archive can reference the loader through shared UI objects.
// This keyboard test must never install process-wide hooks.
extern "C" void *nh_dlhook(void *, const char *, void *)
{
    require(false, "keyboard test attempted to install a loader hook");
    return nullptr;
}

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Export the same metatype interfaces that the adapter checks before allocation.
    require(QMetaType::fromType<KeyboardFrame>().sizeOf() == 40, "frame ABI size");
    require(QMetaType::fromType<KeyboardReceiver>().sizeOf() == 36, "receiver ABI size");
#endif
    QWidget host;
    host.resize(600, 800);
    QLineEdit editor(&host);
    host.show();
    kobalt::NativeKeyboard keyboard(&host, &editor);
    keyboard.setSubmitText(QStringLiteral("Find"));
    require(keyboard.show(), "native keyboard did not open");
    auto *frame = host.findChild<KeyboardFrame *>("rrSearchKeyboard");
    require(frame && frame->isVisible(), "native frame is missing");
    require(frame->geometry() == QRect(0, 560, 600, 240), "keyboard placement");
    require(host.property("reader_keyboard_bounds").toRect() == frame->geometry(),
            "keyboard bounds");
    require(host.property("reader_keyboard_editor_bounds").toRect().isEmpty(),
            "native keyboard inherited custom refresh bounds");
    auto *controller = frame->findChild<SearchKeyboardController *>();
    require(controller && controller->receiver, "receiver was not attached");
    require(controller->receiver->parent() == &editor, "receiver ownership");
    require(controller->property("owned").toBool(), "receiver attach mode");
    require(controller->property("caption").toString() == QStringLiteral("Find"), "submit caption");
    int submitted = 0;
    keyboard.submitted = [&] {
        ++submitted;
        keyboard.hide();
    };
    emit controller->commitRequested();
    emit controller->returnPressed();
    require(submitted == 1, "duplicate submission");
    require(host.property("reader_keyboard_bounds").toRect().isEmpty(), "hidden bounds remained");
    require(keyboard.show(), "keyboard did not reopen");
    emit controller->languageChanged(Symbols);
    QCoreApplication::processEvents();
    require(frame->property("script").toInt() == Symbols,
            "language change did not rebuild keyboard");
    require(frame->isVisible(), "language change hid keyboard");
    controller = frame->findChild<SearchKeyboardController *>();
    emit controller->languageChanged(Latin);
    editor.clearFocus();
    keyboard.hide();
    QCoreApplication::processEvents();
    require(!frame->isVisible(), "a queued language change reopened a dismissed keyboard");
    require(keyboard.show(), "the dismissed keyboard did not reopen explicitly");
    require(frame->property("script").toInt() == Latin,
            "explicit reopening lost the pending language choice");
    keyboard.hide();
    {
        kobalt::NativeKeyboard temporary(&host, &editor);
        require(temporary.show(), "temporary keyboard did not open");
    }
    require(host.property("reader_keyboard_bounds").toRect().isEmpty(),
            "destroyed keyboard bounds remained");
    auto *nested = new QWidget(&host);
    auto *nestedEditor = new QLineEdit(nested);
    auto *nestedKeyboard = new kobalt::NativeKeyboard(nested, nestedEditor);
    nested->show();
    require(nestedKeyboard->show(), "nested keyboard did not open");
    delete nested;
    require(host.property("reader_keyboard_bounds").toRect().isEmpty(),
            "host destruction retained bounds");
    std::puts("PASS: native keyboard construction, ownership, submission, language and cleanup");
}

#include "keyboard.moc"
