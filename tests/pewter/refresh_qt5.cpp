#include "platforms/kobo/nickel-qt5/refresh.h"
#include "platforms/kobo/display/page_animation.h"
#include "platform/api/kobo_refresh.h"
#include "support/diagnostics.h"
#include "check.h"
#include <QApplication>
#include <QBackingStore>
#include <QDialog>
#include <QImage>
#include <array>
#include <cstring>
#include <vector>

namespace {
using Update = std::array<uint32_t, 18>;
std::vector<Update> updates;
int flushes = 0;
int expectImmediate = -1;
bool available = true;

int ioctl(int, unsigned long request, void *argument)
{
    if (request == kobalt::PageAnimation::sendImx) {
        Update update{};
        std::memcpy(update.data(), argument, sizeof(update));
        updates.push_back(update);
    }
    return 0;
}

template <class Flags>
void flush(QBackingStore *, const QRegion &region, QWindow *, const QPoint &, const Flags &flags)
{
    ++flushes;
    for (const auto &entry : flags) {
        if (expectImmediate >= 0)
            require(bool(entry.second & 1) == bool(expectImmediate),
                    "Navigation submission lost its immediate flush flag or changed an unsplit call");
        const auto rect = entry.first;
        require(region.contains(rect), "Native flush policy lies outside submitted damage");
        Update update{ uint32_t(rect.top()),
                       uint32_t(rect.left()),
                       uint32_t(rect.width()),
                       uint32_t(rect.height()),
                       (entry.second & (1U << 13)) ? 10U : 6U,
                       (entry.second & (1U << 3)) ? 1U : 0U,
                       42 };
        kobalt_animation_ioctl(1, kobalt::PageAnimation::sendImx, update.data());
    }
}
} // namespace

namespace kobalt {
PageAnimationSupport pageAnimationSupport()
{
    return available ? PageAnimationSupport::Uncertain : PageAnimationSupport::Unavailable;
}

AnimationPanel pageAnimationPanel()
{
    return AnimationPanel::Mono;
}

void installPageAnimationHook() { }

bool diagnosticsActive()
{
    return false;
}

void diagnostic(const char *, QJsonObject, bool) { }

void diagnostic(const char *, QJsonObject, DiagnosticSeverity) { }

DiagnosticOperation::DiagnosticOperation(const char *name) : operation(name) { }

DiagnosticOperation::~DiagnosticOperation() = default;
} // namespace kobalt

// Checks the Qt5 refresh adapter through both policy-list ABIs: i.MX text sweeps, coverage, full flashes and image exclusions.
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    using namespace kobalt;
    using namespace kobalt::platform;
    nextAnimationIoctl = ioctl;
    nativeFlush = flush<FlushFlags>;
    nativeFlush32 = flush<FlushFlags32>;
    QDialog reader;
    reader.setModal(true);
    reader.resize(300, 400);
    reader.setProperty("reader_refresh_backend", "mxc_epdc_fb");
    reader.setProperty("reader_regal_supported", true);
    reader.setProperty("reader_eclipse_supported", true);
    reader.show();
    app.processEvents();
    QBackingStore store(reader.windowHandle());
    store.resize(reader.size());
    store.beginPaint(reader.rect());
    auto *pixels = static_cast<QImage *>(store.paintDevice());
    *pixels = QImage(reader.size(), QImage::Format_RGB32);
    pixels->fill(Qt::white);
    store.endPaint();
    const QRect damage(10, 20, 280, 360);
    for (bool legacy : { true, false })
        for (bool dark : { false, true }) {
            setReaderRefreshWindow(&reader);
            reader.setProperty("reader_dark", dark);
            const quint32 policy = (dark ? 1U << 13 : 1U << 10) | (1U << 2);
            auto submit = [&] {
                if (legacy)
                    flushReader32(&store, damage, reader.windowHandle(), {},
                                  { { damage, policy } });
                else
                    flushReader(&store, damage, reader.windowHandle(), {}, { { damage, policy } });
            };
            // Entering dark mode clears the screen before ordinary turns.
            submit();
            for (int direction : { -1, 1 }) {
                updates.clear();
                flushes = 0;
                reader.setProperty("reader_animation_direction", direction);
                submit();
                require(flushes == 1 && updates.size() == 10,
                        "Qt5 text turn did not reach the animation hook");
                QRegion coverage;
                for (const auto &update : updates) {
                    require(update[4] == (dark ? 10U : 6U) && update[5] == 0,
                            "Sweep changed the i.MX reading waveform or partial mode");
                    coverage +=
                            QRect(int(update[1]), int(update[0]), int(update[2]), int(update[3]));
                }
                require(coverage == QRegion(reader.rect()),
                        "Qt5 sweep did not cover the completed page");
                require(updates.back()[6] == 42, "Qt5 sweep lost the final update marker");
            }
            for (int excluded : { 0, 1, 2, 3 }) {
                updates.clear();
                flushes = 0;
                available = excluded != 0;
                reader.setProperty("reader_animation_direction", excluded == 1 ? 0 : 1);
                reader.setProperty("reader_full_flash", excluded == 2);
                reader.setProperty(
                        "reader_mono_repairs",
                        QVariant::fromValue(excluded == 3 ? QRegion(damage) : QRegion()));
                submit();
                require(flushes == 1 && updates.size() == 1,
                        "Excluded Qt5 update submitted animation bands");
                if (excluded == 2)
                    require(updates[0][5] == 1, "Full flash lost its full update mode");
            }
            available = true;
            reader.setProperty("reader_mono_repairs", QVariant::fromValue(QRegion()));
        }
    QWidget navigation(&reader);
    navigation.setProperty("reader_navigation_bar", true);
    navigation.setGeometry(0, 340, 300, 60);
    navigation.show();
    reader.setProperty("reader_refresh_backend", "hwtcon");
    const QRect panelAndBar(0, 200, 300, 200);
    for (bool legacy : { true, false }) {
        setReaderRefreshWindow(&reader);
        reader.setProperty("reader_dark", false);
        const quint64 policy = (legacy ? quint64(1) << 10 : quint64(1) << 38) | (1U << 3);
        auto submit = [&] {
            if (legacy)
                flushReader32(&store, panelAndBar, reader.windowHandle(), {},
                              { { panelAndBar, quint32(policy) } });
            else
                flushReader(&store, panelAndBar, reader.windowHandle(), {},
                            { { panelAndBar, policy } });
        };
        updates.clear();
        flushes = 0;
        expectImmediate = 1;
        submit();
        require(flushes == 2 && updates.size() == 2,
                "MTK did not separate the panel and navigation refresh modes");
        QRegion coverage;
        for (const auto &update : updates) {
            const QRect rect{ int(update[1]), int(update[0]), int(update[2]), int(update[3]) };
            require(coverage.intersected(rect).isEmpty(), "MTK navigation damage overlaps");
            coverage += rect;
            require(update[5] == (navigation.geometry().contains(rect) ? 0U : 1U),
                    "MTK flashed unchanged tab pixels or downgraded the panel refresh");
        }
        require(coverage == QRegion(panelAndBar), "MTK navigation split lost painted pixels");
        for (bool plain : { true, false }) {
            reader.setProperty("reader_navigation_dismissal", true);
            reader.setProperty("reader_navigation_dismissal_plain", plain);
            updates.clear();
            flushes = 0;
            submit();
            require(flushes == 1 && updates.size() == 1,
                    "Panel dismissal and tab deselection were submitted separately");
            require(updates[0][5] == (plain ? 0U : 1U),
                    "Dismissal did not preserve text-only versus image cleanup mode");
            require(!reader.property("reader_navigation_dismissal").toBool(),
                    "Navigation dismissal was not consumed");
            updates.clear();
            flushes = 0;
            submit();
            require(flushes == 2 && updates.size() == 2,
                    "An unrelated paint reused the navigation dismissal transaction");
        }
        QWidget overlay(&reader);
        overlay.setGeometry(navigation.geometry());
        overlay.show();
        overlay.raise();
        expectImmediate = 0;
        updates.clear();
        submit();
        require(updates.size() == 1 && updates[0][5] == 1,
                "An overlay inherited the navigation partial refresh");
        overlay.hide();
        navigation.hide();
        updates.clear();
        submit();
        require(updates.size() == 1 && updates[0][5] == 1,
                "Hidden navigation changed the refresh policy");
        navigation.show();
        reader.setProperty("reader_navigation_dismissal", true);
        reader.setProperty("reader_navigation_dismissal_plain", true);
        reader.setProperty("reader_full_flash", true);
        updates.clear();
        submit();
        require(updates.size() == 1 && updates[0][5] == 1,
                "Navigation split an explicit full cleanup");
    }
    for (bool legacy : { true, false }) {
        setReaderRefreshWindow(&reader);
        reader.setProperty("reader_dark", false);
        const QRect keyboard(0, 280, 300, 120), editor(20, 20, 250, 40), key(100, 300, 40, 40);
        reader.setProperty("reader_keyboard_bounds", keyboard);
        reader.setProperty("reader_keyboard_editor_bounds", editor);
        const quint64 policy = (legacy ? quint64(1) << 10 : quint64(1) << 38) | (1U << 3);
        const QRegion edits = QRegion(key) + editor;
        updates.clear();
        flushes = 0;
        expectImmediate = 0;
        if (legacy)
            flushReader32(&store, edits, reader.windowHandle(), {},
                          { { key, quint32(policy) }, { editor, quint32(policy) } });
        else
            flushReader(&store, edits, reader.windowHandle(), {},
                        { { key, policy }, { editor, policy } });
        require(flushes == 1 && updates.size() == 1 && updates[0][5] == 0,
                "Keyboard key and field must submit one partial update through both Qt5 ABIs");
        const auto bounds = edits.boundingRect();
        require(updates[0][0] == uint32_t(bounds.top()) && updates[0][1] == uint32_t(bounds.left())
                        && updates[0][2] == uint32_t(bounds.width())
                        && updates[0][3] == uint32_t(bounds.height()),
                "Keyboard edit update lost key or field damage");
    }
    std::puts("PASS: Qt5 flush ABIs preserve sweeps and keep MTK tab feedback partial");
}
