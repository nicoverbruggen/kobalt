#pragma once
#include <functional>
#include <memory>
class QWidget;
class QString;

namespace kobalt {
// Destroy before the editor and host panel. The reader owns this controller;
// each implementation manages its own keyboard widgets or system input method.
class KeyboardController
{
public:
    virtual ~KeyboardController() = default;
    virtual bool show() = 0;
    virtual void hide() = 0;
    virtual void place() = 0;

    // Set the submit caption before the first show.
    virtual void setSubmitText(const QString &) { }

    std::function<void()> submitted;
};

// The editor is a QLineEdit, or a QTextEdit for text of several lines.
using KeyboardFactory = std::function<std::unique_ptr<KeyboardController>(QWidget *, QWidget *)>;
} // namespace kobalt
