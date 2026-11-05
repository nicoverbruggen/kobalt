#pragma once
#include <functional>
#include <memory>
class QWidget;
class QLineEdit;
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

using KeyboardFactory = std::function<std::unique_ptr<KeyboardController>(QWidget *, QLineEdit *)>;
} // namespace kobalt
