#include "bundled_fonts.h"
#include "assets.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QResource>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFontDatabase>

namespace {
template <class File> QStringList fontFiles(File file) {
    QFile manifest(file("fonts/ebook-core.json"));
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    QStringList result;
    for (const auto &entry : QJsonDocument::fromJson(manifest.readAll()).object().value("files").toArray())
        result.append(file("fonts/ebook-core/" + entry.toObject().value("file").toString()));
    for (const auto *face : {"Regular", "Italic", "Bold"})
        result.append(file(QString("fonts/IBMPlexSans-%1.ttf").arg(face)));
    for (const auto *name : kobalt::scriptFallbackFontFiles)
        result.append(file(QString("fonts/") + QLatin1String(name)));
    return result;
}
}
QStringList kobalt::bundledFontFiles(const AssetBundle &assets) {
    return assets.available() ? fontFiles([&assets](const QString &name) {
        return assets.file(name);
    })
                              : QStringList();
}
kobalt::EngineFontInputs kobalt::engineFontInputs(const AssetBundle &assets, QString systemDirectory,
                                                  QString sideloadDirectory, QString removableRoot) {
    EngineFontInputs inputs;
    if (!assets.available())
        return inputs;
    inputs.sources = {bundledFontFiles(assets), std::move(systemDirectory), std::move(sideloadDirectory),
                      std::move(removableRoot)};
    for (const auto *name : serifFallbackFontFiles) {
        inputs.fallbackFiles.append(assets.file(QString("fonts/") + name));
    }
    for (const auto *name :
         {"IBMPlexMono-Regular.ttf", "IBMPlexMono-Bold.ttf", "NotoSansMath-Regular.ttf", "NotoSerifCJK-Regular.ttc"})
        inputs.fallbackFiles.append(assets.file(QString("fonts/") + name));
    inputs.hyphenationDirectory = assets.file("hyph");
    return inputs;
}
QString kobalt::placeholderFontFamily(const AssetBundle &assets, FontCopies &copies) {
    QString selected;
    for (const auto *face : {"Regular", "Italic", "Bold"}) {
        const auto name = QString("IBMPlexSans-%1.ttf").arg(face);
        const auto bytes = assets.fontData(name);
        const auto path = bytes.isEmpty() ? QString() : copies.file(name, bytes);
        const int id = path.isEmpty() ? -1 : QFontDatabase::addApplicationFont(path);
        if (id < 0)
            continue;
        const auto families = QFontDatabase::applicationFontFamilies(id);
        if (selected.isEmpty() && !families.isEmpty())
            selected = families.first();
    }
    return selected.isEmpty() ? QString("DefaultSansSerif") : selected;
}
