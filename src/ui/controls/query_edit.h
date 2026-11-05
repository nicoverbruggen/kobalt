#pragma once
#include <QGraphicsProxyWidget>
#include <QKeyEvent>
#include <QLineEdit>
#include "ui/style/fonts.h"

namespace kobalt::ui {
class QueryEdit final : public QLineEdit
{
public:
    explicit QueryEdit(QWidget *parent = nullptr) : QLineEdit(parent) { prepareNativeEditor(this); }

protected:
    void keyReleaseEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            // Android can queue a keyboard show on release after submission
            // has already hidden it.
            QWidget::keyReleaseEvent(event);
            return;
        }
        QLineEdit::keyReleaseEvent(event);
    }
};

inline void clearQueryFocus(QLineEdit *query)
{
    if (query->hasFocus()) {
        // Clear the proxy first so Qt delivers FocusOut to its embedded editor
        // and stops the cursor timer.
        for (auto *widget = static_cast<QWidget *>(query); widget;
             widget = widget->parentWidget()) {
            if (auto *proxy = widget->graphicsProxyWidget()) {
                proxy->clearFocus();
                break;
            }
        }
    }
    query->clearFocus();
}
} // namespace kobalt::ui
