#pragma once
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "assets/system_fonts.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "engine/engine_worker.h"
#include "support/qt_compat.h"
#include "ui/style/fonts.h"
#include <QCoreApplication>
#include <QPointer>
#include <atomic>
#include <mutex>
#include <utility>

namespace kobalt::nickel {
inline constexpr auto configDir = KOBALT_CONFIG_DIR;
inline constexpr auto selectionPath = KOBALT_CONFIG_DIR "/config.json";
#ifdef KOBALT_READER_HARDWARE
inline constexpr auto startupPath = KOBALT_CONFIG_DIR "/startup-session";
inline constexpr auto activePath = KOBALT_CONFIG_DIR "/active-session.json";
inline constexpr auto stateReport = KOBALT_CONFIG_DIR "/diagnostics/state.json";
inline constexpr auto sessionReport = KOBALT_CONFIG_DIR "/diagnostics/session.json";
#else
inline constexpr auto activePath = "/instance/kobalt-active";
inline constexpr auto stateReport = "/instance/artifacts/kobalt-state.json";
inline constexpr auto sessionReport = "/instance/artifacts/kobalt-session.json";
#endif

// Nickel owns one environment for the loaded modification. Sessions receive
// values from it; engine and UI code do not look up a platform environment.
struct ReaderEnvironment
{
    explicit ReaderEnvironment(const QString &assetRoot = QStringLiteral(KOBALT_CONFIG_DIR
                                                                         "/assets"))
        : assets(assetRoot, QStringLiteral(KOBALT_VERSION), AssetBundle::Verification::Explicit)
    {
#ifdef KOBALT_READER_HARDWARE
        if (!diagnosticsRequested(QString::fromLatin1(configDir))) {
            paths.stateReport.clear();
            paths.sessionReport.clear();
        }
#endif
    }

    void reportSession(const QJsonObject &report) const
    {
        if (!paths.sessionReport.isEmpty()
            && !sessionReports.tryPost([path = paths.sessionReport, report] {
                   return storage::writeJson(path, report);
               }))
            diagnostic("session-report-worker-unavailable", {}, DiagnosticSeverity::Warning);
    }

    AssetBundle assets;
    std::function<void()> assetsChanged;

    bool verificationForStorage(quint64 permit) const
    {
        return permit && permit == requestedStorage;
    }

    bool assetsAvailable() const
    {
        return (!stockStorage || verificationForStorage(stockStorage())) && assets.available();
    }

    // Called after the stock workflow revokes its permit. A queued worker
    // checks the permit under the same lock before it can touch a file.
    static void finishStockAssetWork()
    {
        mainthread::blocking("stock asset verification");
        std::lock_guard<std::mutex> finished(assetWork());
    }

    // USB calls this only after draining the engine and releasing storage.
    // A cancelled USB cleanup must not invalidate an otherwise usable bundle.
    void invalidateAssets()
    {
        ++assetGeneration;
        requestedStorage = 0;
        preparingOpeningFonts = false;
        cancelOpeningFontWait();
        finishHomeFontWait(false);
        assets.invalidate();
        ui::setPreparedOpeningFonts(assets, {});
        if (assetsChanged)
            assetsChanged();
    }

    // A verified bundle can already admit a tap while its font bytes are still
    // being prepared. Only the active loading surface needs this continuation.
    void whenOpeningFontsReady(std::function<void()> ready)
    {
        // The worker can verify the bundle and then fail to read its fonts.
        // Read them again on the worker instead of refusing every opening
        // until the next USB session or restart.
        if (!preparingOpeningFonts && assetsAvailable() && !ui::openingFontsPrepared(assets))
            readOpeningFontsAgain();
        if (preparingOpeningFonts)
            openingFontsReady = std::move(ready);
        else
            ready();
    }

    void cancelOpeningFontWait() { openingFontsReady = {}; }

    // The Home frame can outlive an opening cancelled by sleep or power-off.
    bool deferHomeFonts(std::function<void(bool)> ready)
    {
        // As for an opening, a failed font read is tried again on the worker.
        if (!preparingOpeningFonts && assetsAvailable() && !ui::openingFontsPrepared(assets))
            readOpeningFontsAgain();
        if (!preparingOpeningFonts)
            return false;
        homeFontsReady = std::move(ready);
        return true;
    }

    void verifyAssets(QObject *context, const char *reason, std::function<void(bool)> finished = {},
                      std::function<quint64()> storageGeneration = {})
    {
        invalidateAssets();
        preparingOpeningFonts = true;
        const auto generation = assetGeneration.load();
        stockStorage = storageGeneration;
        const auto permit = requestedStorage = storageGeneration ? storageGeneration() : 1;
        const auto current = [this, generation, storageGeneration, permit] {
            return generation == assetGeneration.load() && permit
                    && (!storageGeneration || storageGeneration() == permit);
        };
        const QPointer<QObject> guard(context);
        qt::invoke(
                engineWorker(),
                [this, guard, generation, reason, finished, current, storageGeneration, permit] {
                    // Holding this lock never waits for a GUI callback. The queued
                    // result runs separately, after the last asset file is closed.
                    std::lock_guard<std::mutex> reading(assetWork());
                    if (generation != assetGeneration.load())
                        return;
                    DiagnosticOperation operation("asset-verification");
                    const bool valid = current() && assets.verify(current);
                    auto fonts = valid ? ui::readOpeningFonts(assets, current) : ui::OpeningFonts{};
                    qt::invoke(
                            qApp,
                            [this, guard, generation, reason, valid, fonts, finished,
                             storageGeneration, permit] {
                                if (generation != assetGeneration.load())
                                    return;
                                const auto storage =
                                        storageGeneration ? storageGeneration() : permit;
                                if (!storage || storage != permit) {
                                    // Stock USB can start before a coordinator exists. If storage
                                    // already came back, check the new files before resuming a tap.
                                    if (guard && storage) {
                                        auto resume = std::move(openingFontsReady);
                                        verifyAssets(guard, reason, finished, storageGeneration);
                                        openingFontsReady = std::move(resume);
                                    } else {
                                        invalidateAssets();
                                        if (guard && finished)
                                            finished(false);
                                    }
                                    return;
                                }
                                ui::setPreparedOpeningFonts(assets, fonts);
                                preparingOpeningFonts = false;
                                finishHomeFontWait(bool(fonts));
                                auto resume = std::move(openingFontsReady);
                                openingFontsReady = {};
                                if (resume)
                                    resume();
                                if (!guard || generation != assetGeneration.load())
                                    return;
                                diagnostic("assets-checked",
                                           { { "reason", QString::fromLatin1(reason) },
                                             { "valid", valid } },
                                           valid ? DiagnosticSeverity::Activity
                                                 : DiagnosticSeverity::Warning);
                                if (assetsChanged)
                                    assetsChanged();
                                if (finished)
                                    finished(valid);
                            },
                            Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
    }

    FontCopies fontCopies;
    storage::Paths paths{ QString::fromLatin1(configDir),
#ifdef KOBALT_READER_HARDWARE
                          QString::fromLatin1(startupPath),
#else
        {},
#endif
                          QString::fromLatin1(activePath), QString::fromLatin1(stateReport),
                          QString::fromLatin1(sessionReport) };
    EngineFontInputs fonts()
    {
        auto inputs = engineFontInputs(assets, {}, QStringLiteral("/mnt/onboard/fonts"));
        if (inputs.sources.bundled.isEmpty()) {
            return inputs;
        }
        if (!systemFontsReady) {
            systemFonts = systemFontFiles(QStringLiteral(KOBALT_SYSTEM_FONT_DIRECTORY), fontCopies);
            systemFontsReady = true;
        }
        inputs.sources.systemFiles = systemFonts;
        return inputs;
    }

    QString placeholderFamily()
    {
        if (placeholder.isEmpty())
            placeholder = placeholderFontFamily(assets, fontCopies);
        return placeholder;
    }

private:
    static std::mutex &assetWork()
    {
        static std::mutex mutex;
        return mutex;
    }

    // Reads the interface fonts of the current verification again, under the
    // same storage permit. A changed generation means that USB or a new
    // verification took over the opening's wait.
    void readOpeningFontsAgain()
    {
        preparingOpeningFonts = true;
        const auto generation = assetGeneration.load();
        const auto storageGeneration = stockStorage;
        const auto permit = requestedStorage;
        const auto current = [this, generation, storageGeneration, permit] {
            return generation == assetGeneration.load() && permit
                    && (!storageGeneration || storageGeneration() == permit);
        };
        qt::invoke(
                engineWorker(),
                [this, generation, current, storageGeneration, permit] {
                    // The same lock as verification, so that USB waits for this read.
                    std::lock_guard<std::mutex> reading(assetWork());
                    if (generation != assetGeneration.load())
                        return;
                    auto fonts =
                            current() ? ui::readOpeningFonts(assets, current) : ui::OpeningFonts{};
                    qt::invoke(
                            qApp,
                            [this, generation, fonts, storageGeneration, permit] {
                                if (generation != assetGeneration.load())
                                    return;
                                // Stock USB can take the storage before this result arrives.
                                // As after a verification, check remounted files first.
                                const auto storage =
                                        storageGeneration ? storageGeneration() : permit;
                                if (!storage || storage != permit) {
                                    if (storage) {
                                        auto resume = std::move(openingFontsReady);
                                        verifyAssets(qApp, "opening-fonts", {}, storageGeneration);
                                        openingFontsReady = std::move(resume);
                                    } else {
                                        invalidateAssets();
                                    }
                                    return;
                                }
                                diagnostic("opening-fonts-read", { { "ready", bool(fonts) } },
                                           fonts ? DiagnosticSeverity::Activity
                                                 : DiagnosticSeverity::Warning);
                                ui::setPreparedOpeningFonts(assets, fonts);
                                preparingOpeningFonts = false;
                                finishHomeFontWait(bool(fonts));
                                auto resume = std::move(openingFontsReady);
                                openingFontsReady = {};
                                if (resume)
                                    resume();
                            },
                            Qt::QueuedConnection);
                },
                Qt::QueuedConnection);
    }

    std::function<quint64()> stockStorage;
    quint64 requestedStorage = 0;
    std::atomic<quint64> assetGeneration{ 0 };
    bool preparingOpeningFonts = false;
    std::function<void()> openingFontsReady;
    std::function<void(bool)> homeFontsReady;

    void finishHomeFontWait(bool prepared)
    {
        if (const auto ready = std::exchange(homeFontsReady, {}))
            ready(prepared);
    }

    mutable storage::RecordWriter sessionReports;
    QString placeholder;
    QStringList systemFonts;
    bool systemFontsReady = false;
};
} // namespace kobalt::nickel
