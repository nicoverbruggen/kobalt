#pragma once
#include <QByteArray>
#include <QWidget>
#include <QVariant>
#include "framebuffer_driver.h"

namespace kobalt {
enum class RefreshBackend { Unknown, Imx, Mtk };

inline RefreshBackend refreshBackend(QWidget *reader)
{
    // Native scope tests provide a backend without opening a framebuffer.
    const auto selected =
            reader ? reader->property("reader_refresh_backend").toByteArray() : QByteArray();
    const auto driver = selected.isEmpty() ? framebufferDriver() : selected;
    if (mtkFramebuffer(driver))
        return RefreshBackend::Mtk;
    if (imxFramebuffer(driver))
        return RefreshBackend::Imx;
    return RefreshBackend::Unknown;
}
} // namespace kobalt
