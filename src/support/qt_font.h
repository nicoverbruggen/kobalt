#pragma once
#include "qt_compat.h"
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>

namespace kobalt::qt {
template <class Text>
int textWidth(const QFontMetrics &metrics, const Text &text)
{
#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
    return metrics.width(text);
#else
    return metrics.horizontalAdvance(text);
#endif
}

template <class Text>
qreal textWidth(const QFontMetricsF &metrics, const Text &text)
{
#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
    return metrics.width(text);
#else
    return metrics.horizontalAdvance(text);
#endif
}

} // namespace kobalt::qt
