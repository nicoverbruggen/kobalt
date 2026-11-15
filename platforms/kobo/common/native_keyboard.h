#pragma once
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
class QWidget;
enum KeyboardScript : int;

namespace kobalt {
extern void (*nativeKeyboardRepaint)(QWidget *, bool);
void styleNativeKeyboard(QWidget *frame);

// Used only after firmwareSupported() accepts Nickel. The editor is a
// QLineEdit or a QTextEdit; Nickel has a keyboard receiver for each.
class NativeKeyboard : public QObject
{
    Q_OBJECT
public:
    NativeKeyboard(QWidget *parent, QWidget *editor);
    ~NativeKeyboard() override;
    bool show();
    void hide();
    void place();

    void setSubmitText(const QString &text) { submitText = text; }

    std::function<void()> submitted;

protected:
    bool eventFilter(QObject *, QEvent *) override;
private slots:
    void commit();
    void changeScript(KeyboardScript script);

private:
    QPointer<QWidget> frame;
    QPointer<QWidget> editor;
    QPointer<QWidget> host, window;
    QObject *boundsOwner = nullptr; // Identity only; never dereferenced.
    int script = 0;
    bool ready = false;
    QString submitText;
    bool create();
    void publishBounds();
    void clearBounds();
};
} // namespace kobalt
