#pragma once
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "assets/system_fonts.h"
#include "storage/reader_storage.h"
#include "storage/record_writer.h"
#include "support/diagnostics.h"

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
    ReaderEnvironment()
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
        if (!paths.sessionReport.isEmpty())
            sessionReports.post([path = paths.sessionReport,
                                 report] { return storage::writeJson(path, report); },
                                {});
    }

    AssetBundle assets{ QStringLiteral(KOBALT_CONFIG_DIR "/assets"),
                        QStringLiteral(KOBALT_VERSION) };
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
    mutable storage::RecordWriter sessionReports;
    QString placeholder;
    QStringList systemFonts;
    bool systemFontsReady = false;
};
} // namespace kobalt::nickel
