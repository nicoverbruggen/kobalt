#include "custom_pins.h"
#include "support/main_thread.h"
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <lunasvg.h>
#include <cerrno>
#include <cmath>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kobalt::storage {
static_assert(sizeof(ino_t) >= 8 && sizeof(off_t) >= 8,
              "Custom pin discovery requires 64-bit filesystem metadata");

const CustomPin *CustomPins::find(const QString &id) const
{
    for (const auto &icon : icons)
        if (icon.id == id)
            return &icon;
    return nullptr;
}

QImage customPinMask(const QByteArray &svg)
{
    if (svg.isEmpty() || svg.size() > maxCustomPinBytes || svg.contains('\0')
        || svg.toLower().contains("<!doctype") || svg.toLower().contains("<!entity"))
        return {};
    QXmlStreamReader input(svg);
    QByteArray safe;
    QXmlStreamWriter output(&safe);
    int depth = 0, elements = 0, transforms = 0;
    QStringList parents;
    bool root = false;
    const QStringList shapes{ "svg",  "g",        "path",    "rect",  "circle", "ellipse",
                              "line", "polyline", "polygon", "title", "desc" };
    const QStringList attributes{ "viewBox",
                                  "width",
                                  "height",
                                  "d",
                                  "points",
                                  "x",
                                  "y",
                                  "x1",
                                  "y1",
                                  "x2",
                                  "y2",
                                  "cx",
                                  "cy",
                                  "r",
                                  "rx",
                                  "ry",
                                  "fill",
                                  "stroke",
                                  "stroke-width",
                                  "stroke-linecap",
                                  "stroke-linejoin",
                                  "stroke-miterlimit",
                                  "fill-rule",
                                  "transform",
                                  "opacity",
                                  "fill-opacity",
                                  "stroke-opacity",
                                  "version",
                                  "id",
                                  "preserveAspectRatio",
                                  "color" };
    const QRegularExpression number("[-+]?(?:[0-9]+\\.?[0-9]*|\\.[0-9]+)(?:[eE][-+]?[0-9]+)?");
    const QRegularExpression scalar("^" + number.pattern() + "(?:px|%)?$");
    const QRegularExpression coordinate("^" + number.pattern() + "$");
    const QStringList numeric{ "x",
                               "y",
                               "x1",
                               "y1",
                               "x2",
                               "y2",
                               "cx",
                               "cy",
                               "r",
                               "rx",
                               "ry",
                               "width",
                               "height",
                               "stroke-width",
                               "stroke-miterlimit",
                               "opacity",
                               "fill-opacity",
                               "stroke-opacity" };
    while (!input.atEnd()) {
        const auto token = input.readNext();
        if (token == QXmlStreamReader::DTD || token == QXmlStreamReader::EntityReference
            || token == QXmlStreamReader::ProcessingInstruction)
            return {};
        if (input.isStartElement()) {
            const auto name = input.name().toString();
            const auto space = input.namespaceUri().toString();
            if (++depth > 8 || ++elements > 128 || !shapes.contains(name)
                || (!space.isEmpty() && space != "http://www.w3.org/2000/svg")
                || (depth == 1 ? root || name != "svg" : name == "svg"))
                return {};
            root = true;
            parents.append(name);
            output.writeStartElement(name);
            bool viewBox = false;
            for (const auto &attribute : input.attributes()) {
                const auto key = attribute.name().toString(), value = attribute.value().toString();
                // Labels and classes do not affect this renderer without CSS.
                if (attribute.namespaceUri().isEmpty()
                    && (key == "class" || key == "role" || key == "focusable"
                        || key.startsWith("aria-") || key.startsWith("data-")))
                    continue;
                if (!attribute.namespaceUri().isEmpty() || !attributes.contains(key)
                    || value.size() > 4096 || value.contains('&') || value.contains('<')
                    || (value.contains('(') && key != "transform"))
                    return {};
                // Bound geometry before handing it to the renderer. No URL-bearing
                // properties, references, CSS, text, filters or animation reach it.
                const bool geometry = numeric.contains(key) || key == "viewBox" || key == "d"
                        || key == "points" || key == "transform";
                auto numbers = number.globalMatch(geometry ? value : QString());
                int count = 0;
                while (numbers.hasNext()) {
                    bool ok = false;
                    const double n = numbers.next().captured().toDouble(&ok);
                    if (!ok || !std::isfinite(n) || std::abs(n) > 10000 || ++count > 1024)
                        return {};
                }
                if (numeric.contains(key) && !scalar.match(value.trimmed()).hasMatch())
                    return {};
                if (key == "transform" && (transforms += value.count('(')) > 8)
                    return {};
                if (key == "d"
                    && !QRegularExpression("^[MmZzLlHhVvCcSsQqTtAa0-9eE+.,\\s-]*$")
                                .match(value)
                                .hasMatch())
                    return {};
                if (key == "viewBox") {
                    const auto parts = value.trimmed().split(QRegularExpression("[ ,\\t\\r\\n]+"));
                    if (parts.size() != 4)
                        return {};
                    for (const auto &part : parts)
                        if (!coordinate.match(part).hasMatch())
                            return {};
                    if (parts[2].toDouble() < 1 || parts[3].toDouble() < 1
                        || parts[2].toDouble() > 4096 || parts[3].toDouble() > 4096)
                        return {};
                    viewBox = true;
                }
                if (key == "width" || key == "height" || key == "preserveAspectRatio") {
                    // Render at a fixed square size using the viewBox, never source dimensions.
                    if (name == "svg")
                        continue;
                }
                output.writeAttribute(key, value);
            }
            if (depth == 1) {
                if (!viewBox)
                    return {};
                output.writeAttribute("width", "96");
                output.writeAttribute("height", "96");
                output.writeAttribute("preserveAspectRatio", "xMidYMid meet");
            }
        } else if (input.isEndElement()) {
            --depth;
            parents.removeLast();
            output.writeEndElement();
        } else if (input.isCharacters() && !input.isWhitespace()) {
            if (input.text().size() > 1024 || parents.isEmpty()
                || (parents.last() != "title" && parents.last() != "desc"))
                return {};
            // Titles and descriptions are metadata. Never load a font for icon text.
        }
    }
    if (input.hasError() || !root || depth != 0)
        return {};
    auto document = lunasvg::Document::loadFromData(safe.constData(), size_t(safe.size()));
    if (!document)
        return {};
    // Bound the transformed geometry before the rasterizer flattens curves.
    // A small source can otherwise magnify its work through repeated transforms.
    const auto bounds = document->box();
    for (const double value : { bounds.x, bounds.y, bounds.w, bounds.h })
        if (!std::isfinite(value) || std::abs(value) > 4096)
            return {};
    auto bitmap = document->renderToBitmap(96, 96);
    if (!bitmap.data())
        return {};
    bitmap.convertToRGBA();
    QImage mask(96, 96, QImage::Format_ARGB32_Premultiplied);
    bool ink = false;
    for (int y = 0; y < 96; ++y) {
        auto *row = reinterpret_cast<QRgb *>(mask.scanLine(y));
        for (int x = 0; x < 96; ++x) {
            const int alpha = bitmap.data()[y * bitmap.stride() + x * 4 + 3];
            row[x] = qRgba(0, 0, 0, alpha);
            ink |= alpha != 0;
        }
    }
    return ink ? mask : QImage();
}

CustomPins readCustomPins(const QString &directory)
{
    mainthread::BlockingOperation operation("custom-pin-read");
    CustomPins result;
    if (directory.isEmpty())
        return result;
    const int fd = ::open(directory.toUtf8().constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) {
        result.skipped = errno == ENOENT ? 0 : 1;
        return result;
    }
    const auto failed = [&result](const QString &name) {
        ++result.skipped;
        result.failedFiles.append(name);
        result.failedFiles.sort(Qt::CaseSensitive);
        if (result.failedFiles.size() > 5)
            result.failedFiles.removeLast();
    };
    DIR *dir = ::fdopendir(fd);
    if (!dir) {
        ::close(fd);
        result.skipped = 1;
        return result;
    }
    QStringList names;
    const QRegularExpression fileName("^[A-Za-z0-9][A-Za-z0-9_-]{0,43}\\.svg$");
    int entries = 0;
    while (const auto *entry = ::readdir(dir)) {
        const auto name = QString::fromUtf8(entry->d_name);
        if (name == "." || name == "..")
            continue;
        if (++entries > 256) {
            result.truncated = true;
            break;
        }
        if (fileName.match(name).hasMatch())
            names.append(name);
        else if (name.endsWith(".svg", Qt::CaseInsensitive))
            failed(name);
    }
    names.sort(Qt::CaseSensitive);
    for (const auto &name : names) {
        if (result.icons.size() == maxCustomPins) {
            result.truncated = true;
            break;
        }
        // Never follow symlinks or block opening a FIFO. Read through the same
        // directory descriptor even if its pathname changes during discovery.
        const int file =
                ::openat(fd, name.toLatin1().constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        struct stat info{};
        QByteArray bytes;
        if (file >= 0) {
            if (::fstat(file, &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0
                && info.st_size <= maxCustomPinBytes) {
                bytes.resize(maxCustomPinBytes + 1);
                int used = 0;
                while (used < bytes.size()) {
                    const auto n = ::read(file, bytes.data() + used, size_t(bytes.size() - used));
                    if (n < 0) {
                        used = 0;
                        break;
                    }
                    if (n == 0)
                        break;
                    used += int(n);
                }
                bytes.resize(used);
            }
            ::close(file);
        }
        auto mask = customPinMask(bytes);
        if (mask.isNull())
            failed(name);
        else
            result.icons.append({ "custom:" + name, name.left(name.size() - 4), std::move(mask) });
    }
    ::closedir(dir);
    return result;
}
} // namespace kobalt::storage
