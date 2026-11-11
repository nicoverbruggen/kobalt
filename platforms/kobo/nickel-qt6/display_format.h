#pragma once
class QBackingStore;
class QWidget;

namespace kobalt {
void installDisplayFormatHook();
void setDisplayFormatWindow(QWidget *widget);
void verifyDisplayFormat(QBackingStore *store);
} // namespace kobalt
