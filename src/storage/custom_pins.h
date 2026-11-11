#pragma once
#include <QImage>
#include <QStringList>
#include <QVector>

namespace kobalt::storage {
struct CustomPin
{
    QString id, name;
    QImage mask;
};

// Immutable for one book opening. No filesystem access or SVG parsing in paint paths.
struct CustomPins
{
    QVector<CustomPin> icons;
    int skipped = 0;
    QStringList failedFiles; // First five names in alphabetical order.
    bool truncated = false;
    const CustomPin *find(const QString &id) const;
};

inline constexpr int maxCustomPins = 120;
inline constexpr int maxCustomPinBytes = 16 * 1024;
// Worker-only. Does not create the directory or change the user's files.
CustomPins readCustomPins(const QString &directory);
// A deliberately small SVG subset shared by all hosts through LunaSVG.
QImage customPinMask(const QByteArray &svg);
} // namespace kobalt::storage
