#include "reader_p.h"
#include "platform/api/kobo_refresh.h"
#include "app/reader/settings/settings_panel.h"
#include "build_info.h"
#include "reading/rendering_mode.h"
#include "storage/reader_storage.h"
#include "support/diagnostics.h"
#include "support/strings.h"
#include "ui/style/metrics.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSysInfo>
#include <sys/statvfs.h>
#include <unistd.h>

namespace kobalt::reader {
using kobalt::platform::textRefresh;
namespace metrics = ui::metrics;

namespace {
QString bytesText(qint64 value)
{
    if (value < 0)
        return strings::get("debug.unknown");
    const bool gb = value >= 1000000000;
    const double amount = value / (gb ? 1000000000.0 : 1000000.0);
    return QString::number(amount, 'f', amount < 10 ? 1 : 0) + (gb ? " GB" : " MB");
}

// The bytes of the regular files below `directory`. Reads storage.
qint64 folderBytes(const QString &directory)
{
    qint64 total = 0;
    QDirIterator files(directory, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                       QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        total += files.fileInfo().size();
    }
    return total;
}

struct DebugFacts
{
    QString firmware;
    qint64 freeBytes = -1, cacheBytes = -1, logBytes = -1;
};
} // namespace

void Reader::buildDebugSettings()
{
    auto *settings = static_cast<SettingsPanel *>(general);
    connect(settings->debugModeOption, &QCheckBox::toggled, this, [this](bool on) {
        // Fast is offered only in debug mode. Without it a saved "fast"
        // opens as Hybrid, so the draft says so.
        if (!on && settingsRenderingMode == QLatin1String("fast")) {
            settingsRenderingMode = renderingMode::value(renderingMode::Mode::Hybrid);
            showSettingsFonts();
        }
        if (on)
            refreshDebugFacts();
    });
    connect(settings->evictBookCacheButton, &QPushButton::clicked, this,
            [this] { askCacheEviction(false); });
    connect(settings->clearCachesButton, &QPushButton::clicked, this,
            [this] { askCacheEviction(true); });
    connect(settings->detailedDiagnosticsOption, &QCheckBox::clicked, this, [this](bool on) {
        if (diagnosticsMarker().isEmpty())
            return;
        setDetailedDiagnostics(on);
        diagnostic("detailed-diagnostics", { { "enabled", on } }, DiagnosticSeverity::Warning);
        refreshDebugFacts();
    });
    connect(settings->clearDiagnosticLogButton, &QPushButton::clicked, this, [this] {
        clearDiagnosticLog();
        refreshDebugFacts();
    });
    for (auto *fact :
         { settings->debugVersionButton, settings->debugQtButton, settings->debugFirmwareButton,
           settings->debugCoresButton, settings->debugStorageButton })
        connect(fact, &QPushButton::clicked, this, [this] { refreshDebugFacts(); });
}

void Reader::showDebugSettings()
{
    auto *settings = static_cast<SettingsPanel *>(general);
    const bool debug = features::debugMode(preferences);
    // The facts are read once, below, not also by the toggled handler. Qt 5.2
    // has no QSignalBlocker.
    const bool blocked = settings->debugModeOption->blockSignals(true);
    settings->debugModeOption->setChecked(debug);
    settings->debugModeOption->blockSignals(blocked);
    settings->showDebug(debug);
    const bool switchable = !diagnosticsMarker().isEmpty();
    settings->detailedDiagnosticsOption->setChecked(detailedDiagnostics());
    settings->detailedDiagnosticsOption->setEnabled(switchable);
    settings->detailedDiagnosticsOption->setDescription(
            strings::get(switchable ? "debug.detailed_diagnostics_description"
                                    : "debug.detailed_diagnostics_fixed"));
    const auto release = QStringLiteral(KOBALT_VERSION);
    auto version = release.mid(release.startsWith('v') ? 1 : 0);
    if (kobaltCommit[0] != '\0')
        version += QStringLiteral(" · ") + QString::fromLatin1(kobaltCommit)
                + (KOBALT_COMMIT_MODIFIED ? "*" : "");
    settings->debugVersionButton->setCaption(version.toUpper());
    settings->debugQtButton->setCaption(QString::fromLatin1(qVersion()));
    const long online = sysconf(_SC_NPROCESSORS_ONLN), configured = sysconf(_SC_NPROCESSORS_CONF);
    settings->debugCoresButton->setCaption(
            online > 0 && configured > 0
                    ? strings::get("debug.cores_value").arg(online).arg(configured).toUpper()
                    : strings::get("debug.unknown").toUpper());
    if (debug)
        refreshDebugFacts();
}

void Reader::refreshDebugFacts()
{
    // The firmware, the free space and the sizes read storage: the logging
    // worker reads them and the panel shows them when they arrive.
    const auto profile = book->paths().directory;
    const auto caches = QStringList{ book->paths().documentCache(), book->paths().previews() };
    QPointer<Reader> guard(this);
    const bool queued = diagnosticTask([guard, profile, caches] {
        DebugFacts facts;
        facts.firmware = diagnosticStartupFields().value("firmware").toString();
#if QT_VERSION >= QT_VERSION_CHECK(5, 4, 0)
        // Desktop hosts have no firmware. Reads the system's release file.
        if (facts.firmware.isEmpty())
            facts.firmware = QSysInfo::prettyProductName();
#endif
        struct statvfs storage{};
        if (statvfs(QFile::encodeName(profile).constData(), &storage) == 0)
            facts.freeBytes = qint64(storage.f_bavail) * qint64(storage.f_frsize);
        facts.cacheBytes = 0;
        for (const auto &cache : caches)
            facts.cacheBytes += folderBytes(cache);
        facts.logBytes = diagnosticLogBytes();
        kobalt::qt::invoke(
                qApp,
                [guard, facts] {
                    if (!guard || !guard->general)
                        return;
                    auto *settings = static_cast<SettingsPanel *>(guard->general);
                    const auto &firmware = facts.firmware;
                    settings->debugFirmwareButton->setCaption(
                            (firmware.isEmpty() ? strings::get("debug.unknown") : firmware)
                                    .toUpper());
                    settings->debugStorageButton->setCaption(
                            strings::get("debug.storage_value")
                                    .arg(bytesText(facts.freeBytes), bytesText(facts.cacheBytes))
                                    .toUpper());
                    settings->clearDiagnosticLogButton->setCaption(
                            bytesText(facts.logBytes).toUpper());
                },
                Qt::QueuedConnection);
    });
    if (!queued) {
        auto *settings = static_cast<SettingsPanel *>(general);
        settings->debugFirmwareButton->setCaption(strings::get("debug.unknown").toUpper());
        settings->debugStorageButton->setCaption(strings::get("debug.unknown").toUpper());
        settings->clearDiagnosticLogButton->setCaption(strings::get("debug.unknown").toUpper());
    }
}

void Reader::askCacheEviction(bool allBooks)
{
    if (evictionDialog || closing || exitPending)
        return;
    auto *choice = new ui::ChoiceModal(
            this,
            { strings::get(allBooks ? "debug.clear_caches_confirm_description"
                                    : "debug.evict_cache_confirm_description"),
              strings::get(allBooks ? "debug.clear_caches_title" : "debug.evict_cache_title"),
              {},
              "rrCacheEvictionPrompt" },
            { strings::get("common.cancel"), "rrCacheEvictionCancel" },
            { strings::get(allBooks ? "debug.clear" : "debug.evict"), "rrCacheEvictionConfirm" });
    evictionDialog = choice;
    evictionDialog->setObjectName("rrCacheEviction");
    auto finish = [this, allBooks](bool evict) {
        if (!evictionDialog)
            return;
        evictionDialog->hide();
        evictionDialog->deleteLater();
        evictionDialog.clear();
        if (!evict) {
            refresh();
            return;
        }
        // The close saves the place in the book as usual. Instead of keeping
        // the book for a quick return, closeEvent() evicts its cache.
        cacheEviction = allBooks ? 2 : 1;
        diagnostic("document-cache-eviction-requested", { { "all_books", allBooks } },
                   DiagnosticSeverity::Warning);
        close();
    };
    choice->dismissed = [finish] { finish(false); };
    connect(choice->secondaryButton(), &QPushButton::clicked, this, [finish] { finish(false); });
    connect(choice->primaryButton(), &QPushButton::clicked, this, [finish] { finish(true); });
    choice->card()->fitToWidth(
            qMin<int>(metrics::notice::cardWidth, width() - 2 * metrics::notice::sideClearance));
    evictionDialog->centerCard();
    for (auto *widget : evictionDialog->findChildren<QWidget *>())
        textRefresh(widget);
    textRefresh(evictionDialog);
    evictionDialog->show();
    evictionDialog->raise();
    evictionDialog->setFocus();
}
} // namespace kobalt::reader
