#pragma once
#include <QFile>
#include <QString>

namespace kobalt::test {
// Name-only catalog fixtures retain the source glyphs and metrics. Equal-length
// names keep the SFNT table offsets intact; FreeType does not require checksums.
inline bool renamedFontFixture(const QString &source, const QString &destination,
                               const QString &from, const QString &to)
{
    if (from.size() != to.size())
        return false;
    QFile input(source), output(destination);
    if (!input.open(QIODevice::ReadOnly))
        return false;
    auto bytes = input.readAll();
    const auto utf16 = [](const QString &name) {
        QByteArray result;
        for (const auto character : name) {
            result.append(char(character.unicode() >> 8));
            result.append(char(character.unicode() & 255));
        }
        return result;
    };
    if (!bytes.contains(utf16(from)))
        return false;
    bytes.replace(utf16(from), utf16(to));
    bytes.replace(from.toUtf8(), to.toUtf8());
    auto oldPostscript = from, newPostscript = to;
    oldPostscript.replace(' ', '_');
    newPostscript.replace(' ', '_');
    bytes.replace(utf16(oldPostscript), utf16(newPostscript));
    bytes.replace(oldPostscript.toUtf8(), newPostscript.toUtf8());
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size();
}
} // namespace kobalt::test
