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
// `file` maps a manifest name to its path, or to an empty string for an
// asset that is set aside.
template <class File>
QStringList fontFiles(File file)
{
    QFile manifest(file("fonts/ebook-core.json"));
    if (!manifest.open(QIODevice::ReadOnly))
        return {};
    QStringList result;
    const auto add = [&result](const QString &path) {
        if (!path.isEmpty())
            result.append(path);
    };
    for (const auto &entry :
         QJsonDocument::fromJson(manifest.readAll()).object().value("files").toArray())
        add(file("fonts/ebook-core/" + entry.toObject().value("file").toString()));
    for (const auto *face : { "Regular", "Italic", "Bold" })
        add(file(QString("fonts/IBMPlexSans-%1.ttf").arg(face)));
    for (const auto *face : { "Kalam-Regular.ttf", "Kalam-Bold.ttf", "PlaypenSans.ttf" })
        add(file(QStringLiteral("fonts/") + QLatin1String(face)));
    for (const auto *name : kobalt::scriptFallbackFontFiles)
        add(file(QString("fonts/") + QLatin1String(name)));
    return result;
}

QString usableFile(const kobalt::AssetBundle &assets, const QString &name)
{
    return assets.setAside(name) ? QString() : assets.file(name);
}
} // namespace

QStringList kobalt::bundledFontFiles(const AssetBundle &assets)
{
    return assets.available()
            ? fontFiles([&assets](const QString &name) { return usableFile(assets, name); })
            : QStringList();
}

kobalt::EngineFontInputs kobalt::engineFontInputs(const AssetBundle &assets,
                                                  QString systemDirectory,
                                                  QString sideloadDirectory)
{
    EngineFontInputs inputs;
    if (!assets.available())
        return inputs;
    inputs.verifiedFontDigests = assets.verifiedFontDigests();
    inputs.sources = { bundledFontFiles(assets), std::move(systemDirectory),
                       std::move(sideloadDirectory) };
    for (const auto *name : serifFallbackFontFiles) {
        inputs.fallbackFiles.append(assets.file(QString("fonts/") + name));
    }
    for (const auto *name : { "IBMPlexMono-Regular.ttf", "IBMPlexMono-Bold.ttf",
                              "NotoSansMath-Regular.ttf", "NotoSerifCJK-Regular.ttc" }) {
        const auto path = usableFile(assets, QString("fonts/") + name);
        if (!path.isEmpty())
            inputs.fallbackFiles.append(path);
    }
    inputs.hyphenationDirectory = assets.file("hyph");
    inputs.compiledHyphenation = assets.verifiedHyphenation();
    QDir hyphenation(inputs.hyphenationDirectory);
    for (const auto &name : hyphenation.entryList(QDir::Files))
        if (assets.setAside("hyph/" + name))
            inputs.excludedHyphenation.append(name);
    return inputs;
}

QString kobalt::placeholderFontFamily(const AssetBundle &assets, FontCopies &copies)
{
    QString selected;
    for (const auto *face : { "Regular", "Italic", "Bold" }) {
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
