#pragma once
class QWidget;

namespace kobalt::platform {
// Implemented by the selected device integration.
void textRefresh(QWidget *widget);
void pageRefresh(QWidget *widget, bool clean, bool partialText = false);
void setReaderRefreshWindow(QWidget *widget);
} // namespace kobalt::platform
