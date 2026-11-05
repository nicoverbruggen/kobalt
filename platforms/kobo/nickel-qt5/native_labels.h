#pragma once
#include <QLabel>

// Nickel's stylesheet selects these labels by class name, and firmware 4
// exports no constructor for them. Labels with the same class names get
// Nickel's own styling. The plugin's symbols are hidden, so these types do not
// replace Nickel's classes. This header includes nothing else, because Qt 5.2's
// moc cannot parse the C++17 headers the adapters use.
class MetaDataHeaderLabel : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
};
class RegularLabelVPadded : public QLabel {
    Q_OBJECT
public:
    using QLabel::QLabel;
};
