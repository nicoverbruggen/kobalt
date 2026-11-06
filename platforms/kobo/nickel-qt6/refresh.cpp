#include "support/diagnostics.h"
#include "../display/annotation_damage.h"
#include "platform/api/kobo_refresh.h"
#include "refresh.h"
#include "engine/crengine/engine_document.h"
#include "platforms/kobo/display/page_animation.h"
#include "platforms/kobo/display/keyboard_refresh.h"
#include "platforms/kobo/display/panel_damage.h"
#include "platforms/kobo/display/navigation_refresh.h"
#include "platforms/kobo/display/page_animation_hook.h"
#include "platforms/kobo/display/page_transition.h"
#include "platforms/kobo/display/refresh_policy.h"
#include <QPushButton>
#include <QVariant>
#include <QPointer>
#include <QApplication>
#include <QThread>
#include <QBackingStore>
#include <QImage>
#include <QWindow>
#include <algorithm>

namespace kobalt::platform {
LookupRefreshFlags nativeRefreshFlags = nullptr;
Flush nativeFlush = nullptr;

namespace {
QPointer<QWidget> readerWindow, pageWidget;
bool previousDark = false;
KeyboardRefreshCache keyboardPixels;

constexpr quint64 bit(Qt::WidgetAttribute attribute)
{
    return quint64(1) << (attribute - Qt::WA_KoboEpdFlagsStart);
}

quint64 ordinaryPolicy(bool dark)
{
    const auto mode = refreshBackend(readerWindow) == RefreshBackend::Imx
            ? Qt::WA_KoboEpdUpdateModePartial
            : refreshBackend(readerWindow) == RefreshBackend::Mtk
            ? Qt::WA_KoboEpdUpdateModeFull
            : Qt::WA_KoboEpdUpdateModeHeuristic;
    return bit(Qt::WA_KoboEpdWfModeAutoText) | bit(mode)
            | (dark ? bit(Qt::WA_KoboEpdFlagDarkMode) : 0);
}

quint64 repairPolicy(bool dark)
{
    return bit(Qt::WA_KoboEpdWfModeAutoImage) | bit(Qt::WA_KoboEpdUpdateModeFull)
            | (dark ? bit(Qt::WA_KoboEpdFlagDarkMode) : 0);
}

// Firmware 6 stores window pixels as RGBA bytes. Painting an RGB32 page into
// that converts every pixel on the GUI thread, which costs several
// milliseconds per page turn. Every Nickel window passes this hook, so the
// first flush tells the engine which format it should draw before any book opens.
void choosePageFormat(QBackingStore *store)
{
    static bool chosen = false;
    auto *device = store->paintDevice();
    if (chosen || !device || device->devType() != QInternal::Image)
        return;
    chosen = true;
    const auto format = static_cast<QImage *>(device)->format();
    if (format == QImage::Format_RGBA8888 || format == QImage::Format_RGBX8888
        || format == QImage::Format_RGBA8888_Premultiplied)
        EngineDocument::setPageFormat(QImage::Format_RGBX8888);
}

bool readerOwnsScreen()
{
    if (!qApp || QThread::currentThread() != qApp->thread() || !readerWindow
        || !readerWindow->isVisible())
        return false;
    auto *active = QApplication::activeModalWidget();
    return active == readerWindow;
}
} // namespace

void setReaderRefreshWindow(QWidget *widget)
{
#ifdef KOBALT_PAGE_ANIMATIONS
    if (widget)
        installPageAnimationHook();
#endif
    cancelPageAnimation();
    keyboardPixels.clear();
    readerWindow = widget;
    previousDark = false;
}

void flushReader(QBackingStore *store, const QRegion &region, QWindow *target, const QPoint &offset,
                 const FlushFlags &flags)
{
    cancelPageAnimation();
    if (qApp && QThread::currentThread() == qApp->thread() && readerWindow
        && ((readerWindow->property("reader_preparing_handoff").toBool()
             && store->window() != readerWindow->windowHandle())
            || (readerWindow->property("reader_preparing_wake").toBool()
                && store->window() == readerWindow->windowHandle()))
        && (!target || target == store->window()) && offset.isNull())
        return;
    if (!qApp || QThread::currentThread() != qApp->thread()) {
        nativeFlush(store, region, target, offset, flags);
        return;
    }
    choosePageFormat(store);
    if (!readerOwnsScreen() || store->window() != readerWindow->windowHandle()
        || (target && target != store->window()) || !offset.isNull() || region.isEmpty()
        || !readerWindow->rect().contains(region.boundingRect())) {
        keyboardPixels.clear();
        if (readerWindow)
            readerWindow->setProperty("reader_animation_direction", 0);
        nativeFlush(store, region, target, offset, flags);
        return;
    }
    const int direction = readerWindow->property("reader_animation_direction").toInt();
    readerWindow->setProperty("reader_animation_direction", 0);
    const QRect progressUpdate = readerWindow->property("reader_progress_update").toRect();
    readerWindow->setProperty("reader_progress_update", QRect());
    const bool dark = readerWindow->property("reader_dark").toBool();
    const bool pageTurn = readerWindow->property("reader_page_turn").toBool();
    readerWindow->setProperty("reader_page_turn", false);
    const bool navigationDismissal = readerWindow->property("reader_navigation_dismissal").toBool();
    readerWindow->setProperty("reader_navigation_dismissal", false);
    // The reader asks for one full flash to clear ghosting: after the menu
    // closes, and at some chapter starts. It replaces that turn's animation.
    const bool controlsOpening = readerWindow->property("reader_controls_opening").toBool();
    readerWindow->setProperty("reader_controls_opening", false);
    const bool flash = readerWindow->property("reader_full_flash").toBool();
    readerWindow->setProperty("reader_full_flash", false);
    if (flash || (dark && !previousDark)) {
        keyboardPixels.clear();
        // Clear the entire panel, including UI remnants. Entering dark mode
        // always does this.
        previousDark = dark;
        readerWindow->setProperty("reader_annotation_repairs", QVariant::fromValue(QRegion()));
        readerWindow->setProperty("reader_palette_repairs", QVariant::fromValue(QRegion()));
        const QRect screen = readerWindow->rect();
        const auto clean = repairPolicy(dark);
        nativeFlush(store, QRegion(screen), target, offset, { { screen, clean } });
        return;
    }
    previousDark = dark;
    const QRegion annotationRepairs = annotationRepairRegions(readerWindow);
    if (pageAnimationPanel() == AnimationPanel::Color && !(region & annotationRepairs).isEmpty()) {
        keyboardPixels.clear();
        if (pageTurn || direction) {
            // Nickel turns onto and away from coloured highlights with its
            // full-mode reading refresh, without a flash. Edits keep the cleanup.
            const QRect screen = readerWindow->rect();
            nativeFlush(store, QRegion(screen), target, offset,
                        { { screen, ordinaryPolicy(dark) | bit(Qt::WA_KoboEpdScreenFlush) } });
            consumeAnnotationRepairs(readerWindow, QRegion(screen));
        } else {
            nativeFlush(store, region, target, offset,
                        { { region.boundingRect(), repairPolicy(dark) } });
            consumeAnnotationRepairs(readerWindow, region);
        }
        return;
    }
    constexpr auto autoText = bit(Qt::WA_KoboEpdWfModeAutoText),
                   gl16 = bit(Qt::WA_KoboEpdWfModeGL16), gck16 = bit(Qt::WA_KoboEpdWfModeGCK16),
                   darkFlag = bit(Qt::WA_KoboEpdFlagDarkMode);
    constexpr auto heuristic = bit(Qt::WA_KoboEpdUpdateModeHeuristic),
                   full = bit(Qt::WA_KoboEpdUpdateModeFull),
                   partialMode = bit(Qt::WA_KoboEpdUpdateModePartial);
    bool compatible = !flags.isEmpty();
    for (const auto &entry : flags) {
        const auto waveform = entry.second & (autoText | gl16 | gck16);
        const auto mode = entry.second & (heuristic | full | partialMode);
        // A dark cleanup must keep GCK16/full when Qt splits its damage.
        if (dark && waveform == gck16)
            compatible = false;
        if ((entry.second & ~(autoText | gl16 | gck16 | heuristic | full | partialMode | darkFlag))
            || (waveform != autoText && waveform != gl16 && waveform != gck16)
            || (mode != heuristic && mode != full && mode != partialMode)
            || bool(entry.second & darkFlag) != dark)
            compatible = false;
    }
    const bool ordinaryControls = std::all_of(flags.cbegin(), flags.cend(), [&](const auto &entry) {
        return (entry.second & ~(heuristic | full | partialMode))
                == (ordinaryPolicy(dark) & ~(heuristic | full | partialMode));
    });
    if (navigationDismissal && !pageTurn && !direction
        && refreshBackend(readerWindow) == RefreshBackend::Mtk
        && submitNavigationDismissal(
                flags, region, navigationRefreshRegion(readerWindow), ordinaryPolicy(dark),
                repairPolicy(dark), heuristic | full | partialMode, partialMode, full,
                bit(Qt::WA_KoboEpdScreenFlush),
                !dark && readerWindow->property("reader_navigation_dismissal_plain").toBool(),
                [&](const QRegion &damage, const FlushFlags &policies) {
                    nativeFlush(store, damage, target, offset, policies);
                }))
        return;
    if (controlsOpening && compatible && ordinaryControls
        && refreshBackend(readerWindow) == RefreshBackend::Mtk) {
        keyboardPixels.clear();
        const QRect screen = readerWindow->rect();
        auto policy = ordinaryPolicy(dark);
        if (!dark && readerWindow->property("reader_plain_controls").toBool())
            policy = (policy & ~(heuristic | full)) | partialMode;
        nativeFlush(store, QRegion(screen), target, offset, { { screen, policy } });
        return;
    }
    const QRect keyboardBounds = readerWindow->property("reader_keyboard_bounds").toRect();
    const quint64 keyboardPolicy = compatible && ordinaryControls
            ? commonKeyboardPolicy(flags, heuristic | full | partialMode, full, heuristic)
            : 0;
    if (keyboardPolicy) {
        const bool keyboardOnly = std::all_of(flags.cbegin(), flags.cend(), [&](const auto &entry) {
            return keyboardBounds.contains(entry.first);
        });
        if (keyboardPixels.unchanged(dynamic_cast<QImage *>(store->paintDevice()), region,
                                     keyboardBounds, keyboardPolicy, keyboardOnly))
            return;
    } else {
        keyboardPixels.clear();
    }
    if (!dark && !pageTurn && !direction && compatible && ordinaryControls
        && refreshBackend(readerWindow) == RefreshBackend::Mtk
        && keyboardEditDamage(region, keyboardBounds,
                              readerWindow->property("reader_keyboard_editor_bounds").toRect())) {
        const QRect bounds = region.boundingRect();
        const auto policy = (ordinaryPolicy(false) & ~(heuristic | full)) | partialMode;
        nativeFlush(store, QRegion(bounds), target, offset, { { bounds, policy } });
        return;
    }
    const bool progressDamage = !progressUpdate.isEmpty()
            && progressUpdate.contains(region.boundingRect())
            && std::all_of(flags.cbegin(), flags.cend(),
                           [&](const auto &entry) { return progressUpdate.contains(entry.first); });
    auto submitControls = [&](const QRegion &damage, const FlushFlags &policies) {
        if (!dark && !pageTurn && !direction && compatible
            && refreshBackend(readerWindow) == RefreshBackend::Mtk) {
            submitNavigationRefresh(policies, damage, navigationRefreshRegion(readerWindow),
                                    ordinaryPolicy(false), heuristic | full | partialMode,
                                    partialMode, bit(Qt::WA_KoboEpdScreenFlush),
                                    [&](const QRegion &part, const FlushFlags &selected) {
                                        nativeFlush(store, part, target, offset, selected);
                                    });
        } else {
            nativeFlush(store, damage, target, offset, policies);
        }
    };
    auto completedFlush = [&](const QRegion &damage, const FlushFlags &policies) {
        cancelPageAnimation();
        if (direction && compatible && policies.size() == 1
            && policies[0].second == ordinaryPolicy(dark)) {
            if (submitPageTransition(
                        store, readerWindow, direction, dark, ordinaryPolicy(dark),
                        repairPolicy(dark), [&](const QRegion &parts, quint64 policy) {
                            FlushFlags selected;
                            // Submit now so the animation hook runs before the sweep is cancelled.
                            for (const auto &part : parts)
                                selected.append({ part, policy | bit(Qt::WA_KoboEpdScreenFlush) });
                            nativeFlush(store, parts, target, offset, selected);
                        }))
                return;
        }
        if (pageTurn && compatible && refreshBackend(readerWindow) == RefreshBackend::Mtk) {
            const bool color = pageAnimationPanel() == AnimationPanel::Color;
            const QRegion images =
                    readerWindow->property(color ? "reader_image_repairs" : "reader_mono_repairs")
                            .value<QRegion>()
                    & readerWindow->rect();
            // Large pictures and colour turn like Nickel: the reading waveform
            // over the whole screen in full mode, which drives every non-white
            // pixel without the GC16 flash. Other turns stay partial.
            if (!images.isEmpty()
                || (color && readerWindow->property("reader_colour_regions").toBool())) {
                const QRect screen = readerWindow->rect();
                nativeFlush(store, QRegion(screen), target, offset,
                            { { screen, ordinaryPolicy(dark) | bit(Qt::WA_KoboEpdScreenFlush) } });
                return;
            }
        }
        const bool plainControls =
                !dark && readerWindow->property("reader_plain_controls").toBool();
        if (!dark && !pageTurn && !direction && progressDamage
            && refreshBackend(readerWindow) == RefreshBackend::Mtk && compatible
            && policies.size() == 1
            && (policies[0].second == ordinaryPolicy(false)
                || policies[0].second == ((ordinaryPolicy(false) & ~full) | heuristic))) {
            const quint64 policy = (policies[0].second & ~(heuristic | full)) | partialMode;
            nativeFlush(store, damage, target, offset, { { policies[0].first, policy } });
            return;
        }
        if ((pageTurn || plainControls) && !direction && compatible
            && refreshBackend(readerWindow) == RefreshBackend::Mtk && !policies.isEmpty()
            && std::all_of(policies.cbegin(), policies.cend(), [&](const auto &entry) {
                   return entry.second == ordinaryPolicy(dark);
               })) {
            FlushFlags partial = policies;
            for (auto &entry : partial)
                entry.second = (entry.second & ~(heuristic | full)) | partialMode;
            nativeFlush(store, damage, target, offset, partial);
            return;
        }
        // Edits inside an open appearance panel change a slider row or a
        // font sample. Partial mode updates only their changed pixels instead
        // of flashing the whole rectangle. Showing or hiding the whole panel,
        // and the page beside it, keep their policy.
        const QRect controls = readerWindow->property("reader_partial_controls").toRect();
        if (!dark && !pageTurn && !direction && compatible && !controls.isEmpty()
            && refreshBackend(readerWindow) == RefreshBackend::Mtk) {
            FlushFlags partial = policies;
            bool changed = false;
            for (auto &entry : partial) {
                const bool text = entry.second == ordinaryPolicy(false)
                        || entry.second == ((ordinaryPolicy(false) & ~full) | heuristic);
                if (text && controls.contains(entry.first) && entry.first != controls) {
                    entry.second = (entry.second & ~(heuristic | full)) | partialMode;
                    changed = true;
                }
            }
            if (changed) {
                submitControls(damage, partial);
                return;
            }
        }
        if (pageTurn && !direction && compatible
            && refreshBackend(readerWindow) != RefreshBackend::Mtk && policies.size() == 1
            && policies[0].second == ordinaryPolicy(dark)) {
            // Older and unknown backends keep their reading policy over the whole screen.
            const QRect screen = readerWindow->rect();
            nativeFlush(store, QRegion(screen), target, offset, { { screen, policies[0].second } });
            return;
        }
        submitControls(damage, policies);
    };
    QRegion policyCoverage;
    bool uniform = compatible && (region.rectCount() > 1 || flags.size() > 1);
    const QRect bounds = region.boundingRect();
    if (uniform) {
        for (const auto &entry : flags) {
            uniform = uniform && entry.second == flags[0].second && !entry.first.isEmpty()
                    && bounds.contains(entry.first);
            policyCoverage += entry.first;
        }
    }
    if (uniform && region.subtracted(policyCoverage).isEmpty()) {
        // Adjacent widget damage can form one QRegion rectangle while the
        // flag table still has several. Qt has finished painting all of them.
        // Merge here, before MTK submits each rectangle separately. No timer,
        // retained backing-store pointer or later frame is needed.
        const auto defaultText = autoText | heuristic | (dark ? darkFlag : 0);
        const auto policy = flags[0].second == defaultText ? ordinaryPolicy(dark) : flags[0].second;
        // Page transitions keep one rectangle for their existing refresh policy.
        // UI damage keeps one rectangle per panel so unchanged gaps are not refreshed.
        const QRect keyboard = readerWindow->property("reader_keyboard_bounds").toRect();
        const bool keyboardDamage = !keyboard.isEmpty() && keyboard.contains(bounds);
        QRegion damage;
        FlushFlags selected;
        if (pageTurn || direction || keyboardDamage
            || refreshBackend(readerWindow) != RefreshBackend::Mtk) {
            damage = bounds;
            selected.append({ bounds, policy });
        } else {
            for (const auto &part : panelDamage(readerWindow, pageWidget, region)) {
                damage += part;
                selected.append({ part, policy });
            }
        }
        completedFlush(damage, selected);
        return;
    }
    if (compatible && !pageTurn && !direction && refreshBackend(readerWindow) == RefreshBackend::Mtk
        && region.rectCount() > 1 && region.subtracted(policyCoverage).isEmpty()) {
        // Qt can include a window-sized policy rectangle even when its damage
        // has a gap. Submit each panel's painted region with policies clipped to it.
        for (const auto &part : panelDamage(readerWindow, pageWidget, region)) {
            FlushFlags selected;
            for (const auto &entry : flags) {
                const QRect clipped = entry.first.intersected(part);
                if (!clipped.isEmpty() && !selected.contains({ clipped, entry.second }))
                    selected.append({ clipped, entry.second });
            }
            completedFlush(QRegion(part), selected);
        }
        return;
    }
    completedFlush(region, flags);
}

void lookupReaderRefreshFlags(void *screen, quint64 &flags, const QRect &rect)
{
    nativeRefreshFlags(screen, flags, rect);
    // MTK composes sleep during QWindow::hide(), before QWidget's visibility
    // changes. It can reuse the removed reader's dirty flags. Replace those
    // flags only after the native window has left the compositor's stack.
    if (qApp && QThread::currentThread() == qApp->thread() && readerWindow
        && readerWindow->windowHandle() && !readerWindow->windowHandle()->isVisible()
        && (readerWindow->property("reader_sleep_clean_pending").toBool()
            || readerWindow->property("reader_exit_clean_pending").toBool())
        && rect == readerWindow->rect()) {
        readerWindow->setProperty("reader_sleep_clean_pending", false);
        readerWindow->setProperty("reader_exit_clean_pending", false);
        flags = bit(Qt::WA_KoboEpdWfModeGC16) | bit(Qt::WA_KoboEpdUpdateModeFull);
        return;
    }
    constexpr quint64 collide = quint64(1)
            << (Qt::WA_KoboEpdFlagCanCollide - Qt::WA_KoboEpdFlagsStart);
    if (flags != 0 && flags != collide)
        return;
    if (!qApp || QThread::currentThread() != qApp->thread() || !readerWindow
        || !readerWindow->isVisible())
        return;
    auto *active = QApplication::activeModalWidget();
    while (active && active != readerWindow)
        active = active->parentWidget();
    if (!active)
        return;
    // Merged adjacent rectangles can miss Kobo's per-widget flag table. Only
    // repair that empty/default result while our modal reader owns the screen.
    // Explicit clean refreshes and every native window keep their own flags.
    flags |= (quint64(1) << (Qt::WA_KoboEpdWfModeAutoText - Qt::WA_KoboEpdFlagsStart))
            | (quint64(1) << (Qt::WA_KoboEpdUpdateModeHeuristic - Qt::WA_KoboEpdFlagsStart));
    if (readerWindow->property("reader_dark").toBool())
        flags |= quint64(1) << (Qt::WA_KoboEpdFlagDarkMode - Qt::WA_KoboEpdFlagsStart);
}

void textRefresh(QWidget *widget)
{
    if (qobject_cast<QPushButton *>(widget))
        widget->setFocusPolicy(Qt::NoFocus);
    for (int attribute = Qt::WA_KoboEpdUpdateModeHeuristic; attribute <= Qt::WA_KoboEpdWfModeGLK16;
         ++attribute)
        widget->setAttribute(static_cast<Qt::WidgetAttribute>(attribute), false);
    widget->setAttribute(Qt::WA_KoboEpdWfModeAutoImage, false);
    widget->setAttribute(Qt::WA_KoboEpdWfModeAutoText);
    widget->setAttribute(Qt::WA_KoboEpdUpdateModeHeuristic);
    widget->setAttribute(Qt::WA_KoboEpdFlagDarkMode,
                         widget->window()->property("reader_dark").toBool());
}

void pageRefresh(QWidget *widget, bool clean, bool)
{
    const bool dark = widget->window()->property("reader_dark").toBool();
    pageWidget = widget;
    const auto chosen = clean ? repairPolicy(dark) : ordinaryPolicy(dark);
    for (int attribute = Qt::WA_KoboEpdUpdateModeHeuristic; attribute <= Qt::WA_KoboEpdWfModeGLK16;
         ++attribute)
        widget->setAttribute(static_cast<Qt::WidgetAttribute>(attribute), false);
    widget->setAttribute(Qt::WA_KoboEpdWfModeAutoImage, clean);
    widget->setAttribute(Qt::WA_KoboEpdWfModeAutoText, !clean);
    for (const auto mode : { Qt::WA_KoboEpdUpdateModeHeuristic, Qt::WA_KoboEpdUpdateModePartial,
                             Qt::WA_KoboEpdUpdateModeFull })
        widget->setAttribute(mode, bool(chosen & bit(mode)));
    widget->setAttribute(Qt::WA_KoboEpdFlagDarkMode, dark);
}
} // namespace kobalt::platform

extern "C" __attribute__((visibility("default"))) void
kobalt_refresh_flags(void *screen, quint64 &flags, const QRect &rect)
{
    kobalt::platform::lookupReaderRefreshFlags(screen, flags, rect);
}

extern "C" __attribute__((visibility("default"))) void
kobalt_reader_flush(QBackingStore *store, const QRegion &region, QWindow *target,
                    const QPoint &offset, const kobalt::platform::FlushFlags &flags)
{
    kobalt::DiagnosticOperation operation("display-flush");
    kobalt::platform::flushReader(store, region, target, offset, flags);
}
