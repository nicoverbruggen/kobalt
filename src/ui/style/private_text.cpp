#include "private_text.h"
#include "ui/controls/text.h"

#include "fonts.h"
#include "theme.h"
#include "render/highlight_paint.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include "support/diagnostics.h"

#include <QCache>
#include <QFontMetrics>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QHash>
#include <QMap>
#include <QPainter>
#include <QStyle>
#include <QTextBoundaryFinder>
#include <QVector>

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_FONT_FORMATS_H
#include FT_TRUETYPE_TABLES_H
#include <fribidi.h>
#include <hb-ft.h>
#include <hb.h>

namespace kobalt::ui {
namespace {
struct Face
{
    QByteArray bytes;
    FT_Face handle = nullptr;
    QString source;
    // HarfBuzz keeps its table accelerators and shape plans on this font.
    // A font created for each measurement rebuilt them every time.
    hb_font_t *shaper = nullptr;
    FT_Fixed shaperScale[2] = { 0, 0 };

    ~Face()
    {
        if (shaper) {
            hb_font_destroy(shaper);
        }
        if (handle) {
            FT_Done_Face(handle);
        }
    }
};

struct GlyphMask
{
    // Black ink with the glyph's coverage in alpha.
    QImage mask;
    // The bitmap's left bearing and negated top bearing.
    QPoint offset;
    bool missing = false;
    bool drawn = false;
};

struct RegisteredFace
{
    QString file;
    int index;
    bool bold, italic;
};

struct Fonts
{
    FT_Library library = nullptr;
    QMap<QString, QByteArray> sources;
    QMap<QString, qint64> sizes;
    QMap<QString, QString> bundledPaths;
    QStringList recent;
    QSet<QString> pinned;
    int rendering = 0;

    qint64 bytes() const
    {
        qint64 total = 0;
        for (const auto &value : sources)
            total += value.size();
        return total;
    }

    bool reserve(qint64 size)
    {
        if (size <= 0 || size > privateInterfaceFontBudget)
            return false;
        while (bytes() + size > privateInterfaceFontBudget) {
            QString victim;
            for (const auto &file : recent)
                if (!pinned.contains(file) && pending.contains(file)) {
                    victim = file;
                    break;
                }
            if (victim.isEmpty())
                return false;
            // Cached text contains pixels, but primary/glyph lookups contain face pointers.
            clearLayouts();
            glyphs.clear();
            for (auto it = faces.begin(); it != faces.end();) {
                if (it.value()->source == victim) {
                    owners.remove(it.value()->handle);
                    it = faces.erase(it);
                } else
                    ++it;
            }
            const auto released = sources.take(victim).size();
            recent.removeAll(victim);
            setDiagnosticFontBytes(bytes());
            diagnostic("font-cache-evict",
                       { { "released_bytes", released }, { "retained_bytes", double(bytes()) } },
                       false);
        }
        return true;
    }

    void use(const QString &file)
    {
        recent.removeAll(file);
        recent.append(file);
        if (rendering)
            pinned.insert(file);
    }

    QMap<QString, std::function<QByteArray()>> pending;
    // Font previews open bundled fallbacks from their files. These faces
    // retain no bytes, so they do not count against the budget.
    QMap<QString, QString> paths;
    QMap<QString, std::shared_ptr<Face>> faces;
    QHash<FT_Face, Face *> owners;
    // Costs are in 256-byte units, so small measurements do not each take a kilobyte of the budget.
    QCache<QString, PrivateText> layouts{ 8192 };
    // Resolved primary faces and font descriptions, used for every measurement.
    QHash<QString, FT_Face> primaries;
    // Rendered glyphs by face, size, glyph and synthetic emboldening or slant.
    QCache<QByteArray, GlyphMask> glyphs{ 4096 };
    QVector<QPair<QFont, QString>> descriptions;
    QMap<QString, QVector<RegisteredFace>> registered;
    // Registrations already made, by path and expected family.
    QSet<QString> registrations;

    ~Fonts()
    {
        clearFaces();
        if (library) {
            FT_Done_FreeType(library);
        }
    }

    // Faces, registrations and their derived lookups change together.
    void clearLayouts()
    {
        layouts.clear();
        primaries.clear();
    }

    void clearFaces()
    {
        clearLayouts();
        glyphs.clear();
        owners.clear();
        faces.clear();
    }

    FT_Face face(const QString &file, int index = 0)
    {
        const QString key = file + ':' + QString::number(index);
        auto found = faces.find(key);
        if (found != faces.end()) {
            use(file);
            return found.value()->handle;
        }
        if (!sources.contains(file) && paths.contains(file)) {
            auto value = std::make_shared<Face>();
            value->source = file;
            if (FT_New_Face(library, QFile::encodeName(paths.value(file)).constData(), index,
                            &value->handle))
                return nullptr;
            faces.insert(key, value);
            owners.insert(value->handle, value.get());
            return value->handle;
        }
        if (!sources.contains(file)) {
            // An unknown file is a lookup miss, not a rejected load.
            if (!pending.contains(file))
                return nullptr;
            const auto size = sizes.value(file);
            if (!reserve(size)) {
                diagnostic("font-load-rejected",
                           { { "file_bytes", double(size) },
                             { "retained_bytes", double(bytes()) },
                             { "budget_bytes", double(privateInterfaceFontBudget) } },
                           DiagnosticSeverity::Warning);
                return nullptr;
            }
            diagnostic("font-load-start",
                       { { "file_bytes", double(size) }, { "retained_bytes", double(bytes()) } },
                       false);
            const auto read = pending.value(file);
            QByteArray data;
            try {
                data = read ? read() : QByteArray();
            } catch (const std::bad_alloc &) {
                return nullptr;
            }
            if (data.isEmpty() || data.size() != size) {
                diagnostic("font-load-failed", { { "file_bytes", double(size) } },
                           DiagnosticSeverity::Warning);
                return nullptr;
            }
            sources.insert(file, data);
            setDiagnosticFontBytes(bytes());
            diagnostic("font-load-end",
                       { { "file_bytes", double(size) }, { "retained_bytes", double(bytes()) } },
                       false);
        }
        // Even an unreadable face must remain eligible for eviction. A file
        // can change after metadata discovery but keep the same byte size.
        use(file);
        auto value = std::make_shared<Face>();
        value->source = file;
        value->bytes = sources.value(file);
        if (value->bytes.isEmpty()
            || FT_New_Memory_Face(library,
                                  reinterpret_cast<const FT_Byte *>(value->bytes.constData()),
                                  value->bytes.size(), index, &value->handle)) {
            return nullptr;
        }
        faces.insert(key, value);
        owners.insert(value->handle, value.get());
        return value->handle;
    }

    hb_font_t *shaper(FT_Face face)
    {
        auto *owner = owners.value(face);
        if (!owner) {
            return nullptr;
        }
        if (!owner->shaper) {
            owner->shaper = hb_ft_font_create_referenced(face);
            hb_ft_font_set_load_flags(owner->shaper, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
        } else if (owner->shaperScale[0] != face->size->metrics.x_scale
                   || owner->shaperScale[1] != face->size->metrics.y_scale) {
            hb_ft_font_changed(owner->shaper);
        }
        owner->shaperScale[0] = face->size->metrics.x_scale;
        owner->shaperScale[1] = face->size->metrics.y_scale;
        return owner->shaper;
    }

    QString description(const QFont &font)
    {
        for (const auto &entry : descriptions) {
            if (entry.first == font) {
                return entry.second;
            }
        }
        if (descriptions.size() >= 16) {
            descriptions.removeLast();
        }
        descriptions.prepend({ font, font.toString() });
        return descriptions.first().second;
    }
};

int cacheCost(qint64 bytes)
{
    return int(qMin<qint64>(2147483647, qMax<qint64>(1, (bytes + 255) / 256)));
}

thread_local Fonts *previewFonts = nullptr;

Fonts &fonts()
{
    if (previewFonts)
        return *previewFonts;
    static Fonts value;
    return value;
}

// The same glyph at the same size renders the same mask on every line, so
// each one is loaded and rendered by FreeType once.
GlyphMask glyphMask(FT_Face face, uint glyph, FT_Pos strength, bool slant)
{
    struct
    {
        quintptr face;
        FT_Fixed xScale, yScale;
        FT_Pos strength;
        quint32 glyph, slant;
    } raw;

    std::memset(&raw, 0, sizeof(raw));
    raw.face = quintptr(face);
    raw.xScale = face->size->metrics.x_scale;
    raw.yScale = face->size->metrics.y_scale;
    raw.strength = strength;
    raw.glyph = glyph;
    raw.slant = slant;
    const QByteArray key(reinterpret_cast<const char *>(&raw), sizeof(raw));
    if (const auto *cached = fonts().glyphs.object(key)) {
        return *cached;
    }
    GlyphMask result;
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
        result.missing = true;
    } else {
        if (strength && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Outline_Embolden(&face->glyph->outline, strength);
            FT_Outline_Translate(&face->glyph->outline, -strength / 2, -strength / 2);
        }
        if (slant && face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_Matrix matrix{ 0x10000, 0x0366a, 0, 0x10000 };
            FT_Outline_Transform(&face->glyph->outline, &matrix);
        }
        if (FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL)) {
            result.missing = true;
        } else {
            const auto &bitmap = face->glyph->bitmap;
            if (bitmap.width && bitmap.rows
                && (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != 256
                    || !bitmap.buffer || qAbs(qint64(bitmap.pitch)) < bitmap.width
                    || bitmap.width > 16384 || bitmap.rows > 4096
                    || qint64(bitmap.width) * bitmap.rows > 8 * 1024 * 1024)) {
                result.missing = true;
            } else if (bitmap.width && bitmap.rows) {
                result.mask =
                        QImage(bitmap.width, bitmap.rows, QImage::Format_ARGB32_Premultiplied);
                result.missing = result.mask.isNull();
                result.drawn = true;
                result.offset = QPoint(face->glyph->bitmap_left, -face->glyph->bitmap_top);
                for (unsigned y = 0; y < bitmap.rows && !result.mask.isNull(); ++y) {
                    auto *line = reinterpret_cast<QRgb *>(result.mask.scanLine(y));
                    const auto *source = bitmap.buffer + qint64(y) * bitmap.pitch;
                    for (unsigned x = 0; x < bitmap.width; ++x) {
                        line[x] = qRgba(0, 0, 0, source[x]);
                    }
                }
            }
        }
    }
    const int cost = cacheCost(qint64(result.mask.bytesPerLine()) * result.mask.height()
                               + qint64(sizeof(GlyphMask)));
    if (cost <= fonts().glyphs.maxCost()) {
        fonts().glyphs.insert(key, new GlyphMask(result), cost);
    }
    return result;
}

QString primaryFile(const QFont &font)
{
    const bool bold = font.weight() >= QFont::Bold;
    if (font.family().contains("Mono")) {
        return bold ? "IBMPlexMono-Bold.ttf" : "IBMPlexMono-Regular.ttf";
    }
    if (font.family().contains("Serif") && !font.family().contains("Sans")) {
        return font.italic() ? "InstrumentSerif-Italic.ttf" : "InstrumentSerif-Regular.ttf";
    }
    return font.italic() ? "IBMPlexSans-Italic.ttf"
            : bold       ? "IBMPlexSans-Bold.ttf"
                         : "IBMPlexSans-Regular.ttf";
}

bool interfaceFamily(const QFont &font)
{
    return font.family() == "IBM Plex Sans" || font.family() == "IBM Plex Mono"
            || font.family() == "Instrument Serif";
}

const RegisteredFace *registeredFace(const QFont &font)
{
    const auto found = fonts().registered.constFind(font.family());
    if (found == fonts().registered.cend() || found.value().isEmpty()) {
        return nullptr;
    }
    const RegisteredFace *best = &found.value().first();
    int bestScore = -1;
    for (const auto &face : found.value()) {
        const int score = (face.bold == (font.weight() >= QFont::Bold) ? 1 : 0)
                + (face.italic == font.italic() ? 2 : 0);
        if (score > bestScore) {
            best = &face;
            bestScore = score;
        }
    }
    return best;
}

FT_Face resolvePrimaryFace(const QFont &font)
{
    if (previewFonts)
        return previewFonts->faces.value("preview")->handle;
    if (interfaceFamily(font)) {
        return fonts().face(primaryFile(font));
    }
    if (const auto *registered = registeredFace(font)) {
        return fonts().face(registered->file, registered->index);
    }
    return fonts().face(primaryFile(font));
}

// The choice depends only on the family, boldness and slant.
FT_Face primaryFace(const QFont &font)
{
    QString key = font.family();
    key += QLatin1Char(font.weight() >= QFont::Bold ? 'b' : 'r');
    key += QLatin1Char(font.italic() ? 'i' : 'u');
    auto &primaries = fonts().primaries;
    const auto found = primaries.constFind(key);
    if (found != primaries.cend()) {
        const auto face = found.value();
        if (auto *owner = fonts().owners.value(face))
            fonts().use(owner->source);
        return face;
    }
    const auto face = resolvePrimaryFace(font);
    if (face) {
        primaries.insert(key, face);
    }
    return face;
}

FT_Face fallbackFace(const QString &regular, const QFont &font, int index = 0)
{
    if (regular.endsWith("-Regular.ttf")) {
        const auto stem = regular.left(regular.size() - QStringLiteral("Regular.ttf").size());
        QStringList styles;
        if (font.weight() >= QFont::Bold && font.italic()) {
            styles.append("BoldItalic");
        }
        if (font.weight() >= QFont::Bold) {
            styles.append("Bold");
        }
        if (font.italic()) {
            styles.append("Italic");
        }
        for (const auto &style : styles) {
            const auto file = stem + style + ".ttf";
            if (fonts().pending.contains(file) || fonts().sources.contains(file)
                || fonts().paths.contains(file)) {
                if (const auto face = fonts().face(file, index)) {
                    return face;
                }
            }
        }
    }
    return fonts().face(regular, index);
}

FT_Face faceFor(uint character, QChar::Script script, FT_Face primary, const QFont &font)
{
    if (primary && FT_Get_Char_Index(primary, character)) {
        return primary;
    }
    QString preferred;
    int index = 0;
    switch (script) {
    case QChar::Script_Han:
        preferred = "NotoSerifCJK-Regular.ttc";
        index = 2;
        break;
    case QChar::Script_Hangul:
        preferred = "NotoSerifCJK-Regular.ttc";
        index = 1;
        break;
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
        preferred = "NotoSerifCJK-Regular.ttc";
        break;
    case QChar::Script_Arabic:
        preferred = "NotoNaskhArabic-Regular.ttf";
        break;
    case QChar::Script_Hebrew:
        preferred = "NotoSerifHebrew-Regular.ttf";
        break;
    case QChar::Script_Armenian:
        preferred = "NotoSerifArmenian-Regular.ttf";
        break;
    case QChar::Script_Georgian:
        preferred = "NotoSerifGeorgian-Regular.ttf";
        break;
    case QChar::Script_Devanagari:
        preferred = "NotoSerifDevanagari-Regular.ttf";
        break;
    case QChar::Script_Bengali:
        preferred = "NotoSerifBengali-Regular.ttf";
        break;
    case QChar::Script_Tamil:
        preferred = "NotoSerifTamil-Regular.ttf";
        break;
    case QChar::Script_Thai:
        preferred = "NotoSerifThai-Regular.ttf";
        break;
    default:
        break;
    }
    if (!preferred.isEmpty()) {
        const auto face = fallbackFace(preferred, font, index);
        if (face && FT_Get_Char_Index(face, character)) {
            return face;
        }
    }
    for (const auto &file :
         { QStringLiteral("NotoSerif-Regular.ttf"), QStringLiteral("NotoSansMath-Regular.ttf") }) {
        const auto face = fallbackFace(file, font);
        if (face && FT_Get_Char_Index(face, character)) {
            return face;
        }
    }
    return primary;
}

struct Run
{
    int first, count;
    FT_Face face;
    int style;
    QChar::Script script;
    FriBidiLevel level;
    int visual;
};

struct Glyph
{
    QImage mask;
    QPoint position;
};
} // namespace

bool initializePrivateInterfaceText(const AssetBundle &assets, bool opening,
                                    const QMap<QString, QByteArray> &checked)
{
    QMap<QString, std::function<QByteArray()>> pending;
    QStringList files{ "IBMPlexSans-Regular.ttf",    "IBMPlexSans-Bold.ttf",
                       "IBMPlexSans-Italic.ttf",     "IBMPlexMono-Regular.ttf",
                       "IBMPlexMono-Bold.ttf",       "InstrumentSerif-Regular.ttf",
                       "InstrumentSerif-Italic.ttf", "NotoSansMath-Regular.ttf",
                       "NotoSerifCJK-Regular.ttc" };
    for (const auto *file : serifFallbackFontFiles) {
        files.append(QLatin1String(file));
    }
    for (const auto *file : scriptFallbackFontFiles) {
        files.append(QLatin1String(file));
    }
    if (opening) {
        for (const auto &file : files) {
            const auto read = assets.deferredManifestFontData(file);
            if (!read)
                return false;
            pending.insert(file, read);
        }
    } else {
        pending = assets.deferredFontData(files);
        if (pending.isEmpty())
            return false;
    }
    auto &state = fonts();
    if (!state.library && FT_Init_FreeType(&state.library)) {
        return false;
    }
    state.clearFaces();
    state.registered.clear();
    state.registrations.clear();
    state.sources.clear();
    state.sizes.clear();
    state.bundledPaths.clear();
    state.recent.clear();
    state.pinned.clear();
    state.pending = pending;
    for (auto it = pending.cbegin(); it != pending.cend(); ++it) {
        const auto path = assets.file("fonts/" + it.key());
        state.sizes.insert(it.key(), QFileInfo(path).size());
        state.bundledPaths.insert(QFileInfo(path).absoluteFilePath(), it.key());
    }
    for (auto it = checked.cbegin(); it != checked.cend(); ++it)
        if (pending.contains(it.key()) && !it.value().isEmpty())
            state.sources.insert(it.key(), it.value());
    setDiagnosticFontBytes(state.bytes());
    return true;
}

void warmTextShaping(const QByteArray &font, const QString &text)
{
    // A library of its own, so this never touches the GUI thread's faces.
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library))
        return;
    FT_Face face = nullptr;
    if (!FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(font.constData()),
                            FT_Long(font.size()), 0, &face)) {
        FT_Set_Pixel_Sizes(face, 0, 32);
        auto *shaper = hb_ft_font_create_referenced(face);
        auto *buffer = hb_buffer_create();
        hb_buffer_add_utf16(buffer, reinterpret_cast<const uint16_t *>(text.utf16()), text.size(),
                            0, text.size());
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(shaper, buffer, nullptr, 0);
        hb_buffer_destroy(buffer);
        hb_font_destroy(shaper);
        FT_Done_Face(face);
    }
    FT_Done_FreeType(library);
}

qint64 privateInterfaceFontBytes()
{
    qint64 bytes = 0;
    for (const auto &source : fonts().sources)
        bytes += source.size();
    return bytes;
}

QByteArray nativeEditorFontData(const QFont &font)
{
    const auto *registered = registeredFace(font);
    if (!registered)
        return {};
    const auto face = fonts().face(registered->file, registered->index);
    if (!face || face->num_faces != 1 || QByteArray(FT_Get_Font_Format(face)) != "TrueType")
        return {};
    return fonts().sources.value(registered->file);
}

bool registerPrivateInterfaceFont(const QString &path, const QString &expectedFamily)
{
    auto &state = fonts();
    if (!state.library) {
        return false;
    }
    const QString registrationKey = path + '\n' + expectedFamily;
    if (state.registrations.contains(registrationKey)) {
        return true;
    }
    QString key = state.bundledPaths.value(QFileInfo(path).absoluteFilePath());
    if (key.isEmpty())
        key = "reading:" + path;
    const auto size = QFileInfo(path).size();
    if (size <= 0 || size > privateInterfaceFontBudget)
        return false;
    // Inspect one face at a time. Keep metadata, not buffers for every style.
    QVector<QPair<QString, RegisteredFace>> pending;
    FT_Face first = nullptr;
    if (FT_New_Face(state.library, QFile::encodeName(path).constData(), 0, &first))
        return false;
    const auto count = first->num_faces;
    FT_Done_Face(first);
    if (count <= 0 || count > 256)
        return false;
    for (int index = 0; index < count; ++index) {
        FT_Face face = nullptr;
        if (FT_New_Face(state.library, QFile::encodeName(path).constData(), index, &face))
            return false;
        const QString family = QString::fromUtf8(face->family_name);
        const RegisteredFace entry{ key, index, bool(face->style_flags & FT_STYLE_FLAG_BOLD),
                                    bool(face->style_flags & FT_STYLE_FLAG_ITALIC) };
        FT_Done_Face(face);
        pending.append({ family, entry });
        if (!expectedFamily.isEmpty() && expectedFamily != family && count == 1)
            pending.append({ expectedFamily, entry });
    }
    if (!state.pending.contains(key)) {
        state.sizes.insert(key, size);
        state.pending.insert(key, [path, size] {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() != size)
                return QByteArray();
            // Bound the read even if a file grows after the size check.
            auto data = file.read(size);
            return data.size() == size ? data : QByteArray();
        });
    }
    for (const auto &value : pending) {
        auto &entries = state.registered[value.first];
        bool present = false;
        for (const auto &entry : entries) {
            if (entry.file == value.second.file && entry.index == value.second.index) {
                present = true;
                break;
            }
        }
        if (!present) {
            entries.append(value.second);
        }
    }
    state.clearLayouts();
    state.registrations.insert(registrationKey);
    return true;
}

bool privateFontFixedPitch(const QFont &font)
{
    const auto face = primaryFace(font);
    return face && FT_IS_FIXED_WIDTH(face);
}

bool needsPrivateText(const QString &text, const QFont &)
{
    // Native drawing is only a bootstrap fallback before verified font bytes
    // are available. Every application text surface uses the retained fonts.
    return fonts().library && !text.isEmpty();
}

static PrivateText layoutPrivateText(const QString &input, const QFont &font, int weight,
                                     const QVector<QPair<int, int>> &boldRanges, bool rasterize,
                                     const QVector<QPair<int, int>> &underlineRanges = {},
                                     PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                                     Qt::LayoutDirection direction = Qt::LayoutDirectionAuto,
                                     const QVector<PrivateTextStyle> &styles = {})
{
    struct Rendering
    {
        Fonts &state;

        Rendering(Fonts &value) : state(value) { ++state.rendering; }

        ~Rendering()
        {
            if (!--state.rendering)
                state.pinned.clear();
        }
    } rendering(fonts());

    QString text = input;
    if (font.capitalization() == QFont::AllUppercase) {
        text = text.toUpper();
    } else if (font.capitalization() == QFont::AllLowercase) {
        text = text.toLower();
    }
    PrivateText result;
    if (text.isEmpty() || !fonts().library) {
        return result;
    }
    const bool interface = !previewFonts
            && (interfaceFamily(font) || font.family() == interfaceFont().family()
                || font.family() == chromeFont().family()
                || font.family() == headerFooterFont().family());
    const int steps = weight < 0 ? (interface ? interfaceWeight() : 0) : qBound(0, weight, 12);
    QString cacheKey = fonts().description(font) + ':' + QString::number(steps) + ':'
            + QString::number(rasterize) + ':' + QString::number(font.capitalization()) + ':'
            + QString::number(font.letterSpacingType()) + ':'
            + QString::number(font.letterSpacing(), 'g', 16) + ':'
            + QString::number(font.wordSpacing(), 'g', 16) + ':' + QString::number(font.stretch())
            + ':' + QString::number(font.kerning());
    for (const auto &range : boldRanges) {
        cacheKey += ':' + QString::number(range.first) + ',' + QString::number(range.second);
    }
    cacheKey +=
            ":underline:" + QString::number(int(emphasis)) + ':' + QString::number(int(direction));
    for (const auto &range : underlineRanges) {
        cacheKey += ':' + QString::number(range.first) + ',' + QString::number(range.second);
    }
    for (const auto &style : styles) {
        cacheKey += ":style:" + QString::number(style.start) + ',' + QString::number(style.length)
                + ':' + fonts().description(style.font) + ':' + style.color.name(QColor::HexArgb)
                + ':' + QString::number(style.baselineShift) + ':'
                + QString::number(style.image.cacheKey());
        if (style.background.isValid())
            cacheKey += ":background:" + style.background.name(QColor::HexArgb);
    }
    cacheKey += ':' + text;
    if (const auto *cached = fonts().layouts.object(cacheKey)) {
        return *cached;
    }
    const auto characters = text.toUcs4();
    const int count = characters.size();
    QVector<int> utf16Offsets;
    utf16Offsets.reserve(count);
    int utf16 = 0;
    for (const auto character : characters) {
        utf16Offsets.append(utf16);
        utf16 += character > 0xffff ? 2 : 1;
    }
    QVector<FriBidiLevel> levels(count);
    QVector<FriBidiStrIndex> logicalToVisual(count);
    FriBidiParType base = direction == Qt::RightToLeft ? FRIBIDI_PAR_RTL
            : direction == Qt::LeftToRight             ? FRIBIDI_PAR_LTR
                                                       : FRIBIDI_PAR_ON;
    // Without right-to-left, Arabic number or explicit directional
    // characters, every character stays in logical order at level zero.
    // FriBidi's own types decide, so the skip always agrees with FriBidi.
    bool reorder = direction == Qt::RightToLeft;
    for (int i = 0; i < count && !reorder; ++i) {
        const auto type = fribidi_get_bidi_type(characters[i]);
        reorder = FRIBIDI_IS_RTL(type) || FRIBIDI_IS_ARABIC(type) || FRIBIDI_IS_EXPLICIT(type)
                || FRIBIDI_IS_ISOLATE(type);
    }
    if (!reorder) {
        for (int i = 0; i < count; ++i) {
            logicalToVisual[i] = i;
        }
    } else if (!fribidi_log2vis(characters.constData(), count, &base, nullptr,
                                logicalToVisual.data(), nullptr, levels.data())) {
        return result;
    }
    std::vector<Run> runs;
    const auto primary = primaryFace(font);
    QFont boldFont = font;
    boldFont.setWeight(QFont::Bold);
    const auto boldPrimary =
            emphasis == PrivateTextEmphasis::FontFace ? primaryFace(boldFont) : primary;
    QVector<FT_Face> styleFaces;
    styleFaces.reserve(styles.size());
    for (const auto &style : styles) {
        styleFaces.append(primaryFace(style.font));
    }
    QVector<FT_Face> characterFaces(count, nullptr);
    QVector<int> characterStyles(count, -1);
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    boundaries.toStart();
    int firstCharacter = 0;
    for (int start = 0, end = boundaries.toNextBoundary(); end >= 0;
         start = end, end = boundaries.toNextBoundary()) {
        // The cluster's code points are already decoded in characters.
        const uint *cluster = characters.constData() + firstCharacter;
        int clusterSize = 0;
        while (firstCharacter + clusterSize < count
               && utf16Offsets[firstCharacter + clusterSize] < end) {
            ++clusterSize;
        }
        if (!clusterSize) {
            continue;
        }
        auto covers = [&](FT_Face face) {
            if (!face) {
                return false;
            }
            for (int i = 0; i < clusterSize; ++i) {
                const auto character = cluster[i];
                if (character != '\t' && QChar::category(character) != QChar::Other_Format
                    && !FT_Get_Char_Index(face, character)) {
                    return false;
                }
            }
            return true;
        };
        FT_Face requested = primary;
        const QFont *requestedFont = &font;
        int styleIndex = -1;
        for (int i = 0; i < styles.size(); ++i)
            if (start >= styles[i].start && start < styles[i].start + styles[i].length) {
                styleIndex = i;
                requestedFont = &styles[i].font;
                requested = styleFaces[i];
                break;
            }
        for (const auto &range : boldRanges) {
            if (start >= range.first && start < range.first + range.second) {
                requested = boldPrimary;
                if (emphasis == PrivateTextEmphasis::FontFace) {
                    requestedFont = &boldFont;
                }
                break;
            }
        }
        FT_Face face = requested;
        const bool image = styleIndex >= 0 && !styles[styleIndex].image.isNull();
        if (!image && !covers(face)) {
            face = faceFor(cluster[0], QChar::script(cluster[0]), requested, *requestedFont);
            if (!covers(face)) {
                QStringList files = fonts().pending.keys();
                for (const auto &file : fonts().sources.keys() + fonts().paths.keys())
                    if (!files.contains(file))
                        files.append(file);
                for (const auto &file : files) {
                    if (file.endsWith("-Bold.ttf") || file.endsWith("-Italic.ttf")
                        || file.endsWith("-BoldItalic.ttf")) {
                        const auto regular = file.left(file.lastIndexOf('-') + 1) + "Regular.ttf";
                        if (fonts().pending.contains(regular) || fonts().sources.contains(regular)
                            || fonts().paths.contains(regular)) {
                            continue;
                        }
                    }
                    const auto candidate = fallbackFace(file, *requestedFont);
                    if (covers(candidate)) {
                        face = candidate;
                        break;
                    }
                }
            }
        }
        for (int i = 0; i < clusterSize; ++i) {
            characterFaces[firstCharacter] = face;
            characterStyles[firstCharacter++] = styleIndex;
        }
    }
    QChar::Script previousScript = QChar::Script_Latin;
    for (int i = 0; i < count; ++i) {
        auto script = QChar::script(characters[i]);
        if (script == QChar::Script_Common || script == QChar::Script_Inherited) {
            script = previousScript;
        }
        previousScript = script;
        const auto face = characterFaces[i];
        if (!face) {
            return result;
        }
        if (!runs.empty() && runs.back().face == face && runs.back().script == script
            && runs.back().level == levels[i] && runs.back().style == characterStyles[i]) {
            ++runs.back().count;
            runs.back().visual = qMin(runs.back().visual, int(logicalToVisual[i]));
        } else {
            runs.push_back(
                    { i, 1, face, characterStyles[i], script, levels[i], int(logicalToVisual[i]) });
        }
    }
    std::sort(runs.begin(), runs.end(),
              [](const Run &a, const Run &b) { return a.visual < b.visual; });
    const int pixels = font.pixelSize() > 0 ? font.pixelSize() : 32;
    QVector<Glyph> glyphs;
    QVector<QPair<QRect, QColor>> underlines;
    QVector<QPair<QRect, QColor>> backgrounds;
    QRect bounds;
    qint64 pen = 0;
    int maximumLineDescent = 0;
    uint previousCluster = uint(-1);
    const auto retainBackground = [&](qint64 start, const PrivateTextStyle *style) {
        if (rasterize && style && style->background.isValid() && pen > start) {
            const int left = int(start / 64), right = int((pen + 63) / 64);
            backgrounds.append({ QRect(left, 0, right - left, 0), style->background });
        }
    };
    for (const auto &run : runs) {
        const qint64 start = pen;
        const auto *style = run.style < 0 ? nullptr : &styles[run.style];
        const auto &runFont = style ? style->font : font;
        if (style && !style->image.isNull()) {
            const auto &image = style->image;
            result.lineAscent = qMax(result.lineAscent, image.height());
            result.height = qMax(result.height, image.height());
            result.lineHeight = result.lineAscent + maximumLineDescent;
            for (int object = 0; object < run.count; ++object) {
                if (rasterize) {
                    const QPoint origin(int(pen / 64), -image.height());
                    bounds = bounds.united(QRect(origin, image.size()));
                    glyphs.push_back({ image, origin });
                }
                pen += qint64(image.width()) * 64;
            }
            retainBackground(start, style);
            continue;
        }
        const int pixels = runFont.pixelSize() > 0 ? runFont.pixelSize() : 32;
        const int shift = style ? style->baselineShift : 0;
        const auto ink = style && style->color.isValid() ? style->color : QColor(Qt::black);
        if (FT_Set_Pixel_Sizes(
                    run.face,
                    qMax(1, pixels * (runFont.stretch() > 0 ? runFont.stretch() : 100) / 100),
                    pixels)) {
            return {};
        }
        result.height =
                qMax(result.height,
                     int((run.face->size->metrics.ascender - run.face->size->metrics.descender + 63)
                         / 64));
        int lineAscent = int((run.face->size->metrics.ascender + 63) / 64);
        int lineDescent = int((-run.face->size->metrics.descender + 63) / 64);
        // Older Qt uses the horizontal header for document line boxes. Keep
        // excerpt baselines and spacing when FreeType selects OS/2 typo metrics.
        const auto *header =
                static_cast<const TT_HoriHeader *>(FT_Get_Sfnt_Table(run.face, ft_sfnt_hhea));
        if (header && header->Ascender > 0 && header->Descender <= 0) {
            lineAscent =
                    int((FT_MulFix(header->Ascender, run.face->size->metrics.y_scale) + 63) / 64);
            lineDescent =
                    int((-FT_MulFix(header->Descender, run.face->size->metrics.y_scale) + 63) / 64);
        }
        result.lineAscent = qMax(result.lineAscent, lineAscent - shift);
        maximumLineDescent = qMax(maximumLineDescent, lineDescent + shift);
        result.lineHeight = result.lineAscent + maximumLineDescent;
        hb_font_t *hbFont = fonts().shaper(run.face);
        if (!hbFont) {
            return {};
        }
        hb_buffer_t *buffer = hb_buffer_create();
        hb_buffer_add_utf32(buffer, characters.constData(), count, run.first, run.count);
        hb_buffer_guess_segment_properties(buffer);
        hb_buffer_set_direction(buffer, run.level & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_feature_t features[3];
        hb_feature_from_string(runFont.kerning() ? "kern=1" : "kern=0", -1, &features[0]);
        const bool separateLetters = !boldRanges.isEmpty() || !underlineRanges.isEmpty()
                || (runFont.letterSpacingType() == QFont::AbsoluteSpacing
                    && runFont.letterSpacing() != 0);
        if (separateLetters) {
            hb_feature_from_string("liga=0", -1, &features[1]);
            hb_feature_from_string("clig=0", -1, &features[2]);
        }
        hb_shape(hbFont, buffer, features, separateLetters ? 3 : 1);
        auto runUnderlines = underlineRanges;
        if (runFont.underline())
            runUnderlines.append({ 0, int(text.size()) });
        unsigned glyphCount = 0;
        const auto *info = hb_buffer_get_glyph_infos(buffer, &glyphCount);
        const auto *positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);
        for (unsigned i = 0; i < glyphCount; ++i) {
            if (previousCluster != uint(-1) && previousCluster != info[i].cluster
                && runFont.letterSpacingType() == QFont::AbsoluteSpacing) {
                pen += qRound64(runFont.letterSpacing() * 64);
            }
            previousCluster = info[i].cluster;
            if (info[i].cluster < uint(count) && characters[int(info[i].cluster)] == '\t') {
                // Preformatted text and licenses share eight-space tab stops.
                // A control character has spacing, never a font's missing glyph.
                const qint64 stop =
                        qint64(qMax(1, measurePrivateText(QStringLiteral("        "), runFont)))
                        * 64;
                pen = (pen / stop + 1) * stop;
                continue;
            }
            bool bold = runFont.weight() >= QFont::Bold
                    && !(run.face->style_flags & FT_STYLE_FLAG_BOLD);
            for (const auto &range : boldRanges) {
                if (utf16Offsets[int(info[i].cluster)] >= range.first
                    && utf16Offsets[int(info[i].cluster)] < range.first + range.second) {
                    bold = true;
                    break;
                }
            }
            if (emphasis == PrivateTextEmphasis::FontFace
                && (run.face->style_flags & FT_STYLE_FLAG_BOLD)) {
                bold = false;
            }
            if (!info[i].codepoint) {
                ++result.missingGlyphs;
            }
            if (rasterize) {
                if (runFont.strikeOut()) {
                    const QRect strike(int(pen / 64), shift - lineAscent / 3,
                                       qMax(1, int((positions[i].x_advance + 63) / 64)),
                                       qMax(1, pixels / 16));
                    bounds = bounds.united(strike);
                    underlines.append({ strike, ink });
                }
                for (const auto &range : runUnderlines) {
                    if (utf16Offsets[int(info[i].cluster)] >= range.first
                        && utf16Offsets[int(info[i].cluster)] < range.first + range.second) {
                        const int position = qMax(1,
                                                  int(-FT_MulFix(run.face->underline_position,
                                                                 run.face->size->metrics.y_scale)
                                                      / 64));
                        const int thickness = qMax(1,
                                                   int((FT_MulFix(run.face->underline_thickness,
                                                                  run.face->size->metrics.y_scale)
                                                        + 63)
                                                       / 64));
                        const QRect underline(int(pen / 64), position + shift,
                                              qMax(1, int((positions[i].x_advance + 63) / 64)),
                                              thickness);
                        bounds = bounds.united(underline);
                        underlines.append({ underline, ink });
                        break;
                    }
                }
            }
            if (rasterize && pen < 16384 * 64) {
                const FT_Pos strength = !steps && !bold ? 0
                        : bold ? qMax(pixels * 64 * steps / 256, pixels * 64 / 24)
                               : pixels * 64 * steps / 256;
                const bool slant =
                        runFont.italic() && !(run.face->style_flags & FT_STYLE_FLAG_ITALIC);
                const auto glyph = glyphMask(run.face, info[i].codepoint, strength, slant);
                if (glyph.missing) {
                    ++result.missingGlyphs;
                }
                if (glyph.drawn) {
                    auto mask = glyph.mask;
                    const QPoint origin((pen + positions[i].x_offset) / 64 + glyph.offset.x(),
                                        -positions[i].y_offset / 64 + glyph.offset.y() + shift);
                    if (ink != Qt::black && !mask.isNull()) {
                        QPainter color(&mask);
                        color.setCompositionMode(QPainter::CompositionMode_SourceIn);
                        color.fillRect(mask.rect(), ink);
                    }
                    bounds = bounds.united(QRect(origin, mask.size()));
                    glyphs.append({ mask, origin });
                }
            }
            pen += runFont.letterSpacingType() == QFont::PercentageSpacing
                    ? qRound64(positions[i].x_advance
                               * (runFont.letterSpacing() == 0 ? 100 : runFont.letterSpacing())
                               / 100.0)
                    : positions[i].x_advance;
            if (info[i].cluster < uint(count) && QChar::isSpace(characters[int(info[i].cluster)])) {
                pen += qRound64(runFont.wordSpacing() * 64);
            }
        }
        hb_buffer_destroy(buffer);
        retainBackground(start, style);
    }
    result.advance = int(qMin<qint64>(2147483647, qMax<qint64>(0, (pen + 63) / 64)));
    if (!rasterize) {
        const int cost = cacheCost(qint64(cacheKey.size()) * 2 + qint64(sizeof(PrivateText)));
        if (cost <= fonts().layouts.maxCost()) {
            fonts().layouts.insert(cacheKey, new PrivateText(result), cost);
        }
        return result;
    }
    bounds = bounds.united(QRect(0, -pixels, result.advance, pixels));
    for (auto &background : backgrounds) {
        background.first = QRect(background.first.x(), -result.lineAscent, background.first.width(),
                                 result.lineHeight);
        bounds = bounds.united(background.first);
    }
    if (bounds.isEmpty() || bounds.width() > 16384 || bounds.height() > 4096
        || qint64(bounds.width()) * bounds.height() > 8 * 1024 * 1024) {
        return result;
    }
    result.image = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    result.origin = bounds.topLeft();
    result.image.fill(Qt::transparent);
    QPainter painter(&result.image);
    for (const auto &background : backgrounds)
        painter.fillRect(background.first.translated(-bounds.topLeft()), background.second);
    for (const auto &glyph : glyphs) {
        painter.drawImage(glyph.position - bounds.topLeft(), glyph.mask);
    }
    for (const auto &underline : underlines) {
        painter.fillRect(underline.first.translated(-bounds.topLeft()), underline.second);
    }
    painter.end();
    const int cost = cacheCost(qint64(result.image.bytesPerLine()) * result.image.height()
                               + qint64(cacheKey.size()) * 2 + qint64(sizeof(PrivateText)));
    if (cost <= fonts().layouts.maxCost()) {
        fonts().layouts.insert(cacheKey, new PrivateText(result), cost);
    }
    return result;
}

PrivateText renderPrivateText(const QString &text, const QFont &font, int weight,
                              const QVector<QPair<int, int>> &boldRanges,
                              const QVector<QPair<int, int>> &underlineRanges,
                              PrivateTextEmphasis emphasis, Qt::LayoutDirection direction,
                              const QVector<PrivateTextStyle> &styles)
{
    return layoutPrivateText(text, font, weight, boldRanges, true, underlineRanges, emphasis,
                             direction, styles);
}

QMap<QString, QString> privateInterfaceFontFiles()
{
    QMap<QString, QString> result;
    const auto &paths = fonts().bundledPaths;
    for (auto it = paths.cbegin(); it != paths.cend(); ++it)
        result.insert(it.value(), it.key());
    return result;
}

QImage renderFontPreviewBitmap(const QStringList &files, const QString &family, const QString &text,
                               QSize size, const FontPreviewScale &scale, int weight,
                               bool boldReading, bool centered, bool wrap,
                               const QMap<QString, QString> &fallbacks)
{
    if (text.isEmpty() || text.size() > 256 || files.size() > 64 || size.width() <= 0
        || size.height() <= 0 || size.width() > 2048 || size.height() > 512
        || qint64(size.width()) * size.height() > 256 * 1024)
        return {};
    Fonts isolated;
    if (FT_Init_FreeType(&isolated.library))
        return {};
    isolated.glyphs.setMaxCost(512);
    isolated.layouts.setMaxCost(1024);
    // Characters the face lacks use the same bundled fallbacks as the
    // interface renderer, so a script font can still show a Latin name.
    isolated.paths = fallbacks;
    QString chosen;
    int chosenIndex = 0, best = -1;
    // Collections and unnamed styles need metadata inspection. Close each face
    // before looking at the next. No QByteArray contains the whole font file.
    for (const auto &path : files) {
        const auto bytes = QFileInfo(path).size();
        if (bytes <= 0 || bytes > 64LL * 1024 * 1024)
            continue;
        const auto encoded = QFile::encodeName(path);
        int count = 1;
        for (int index = 0; index < count; ++index) {
            FT_Face face = nullptr;
            if (FT_New_Face(isolated.library, encoded.constData(), index, &face))
                break;
            count = qBound(1, int(face->num_faces), 256);
            const int score = (QString::fromUtf8(face->family_name) == family ? 4 : 0)
                    + (!(face->style_flags & FT_STYLE_FLAG_BOLD) ? 1 : 0)
                    + (!(face->style_flags & FT_STYLE_FLAG_ITALIC) ? 2 : 0);
            FT_Done_Face(face);
            if (score > best) {
                best = score;
                chosen = path;
                chosenIndex = index;
            }
            if (best == 7)
                break;
        }
        if (best == 7)
            break;
    }
    if (chosen.isEmpty())
        return {};
    auto face = std::make_shared<Face>();
    if (FT_New_Face(isolated.library, QFile::encodeName(chosen).constData(), chosenIndex,
                    &face->handle))
        return {};
    isolated.faces.insert("preview", face);
    isolated.owners.insert(face->handle, face.get());

    struct Scope
    {
        Fonts *previous;

        explicit Scope(Fonts *state) : previous(previewFonts) { previewFonts = state; }

        ~Scope() { previewFonts = previous; }
    } scope(&isolated);

    QFont font(family);
    font.setPixelSize(128);
    // Measure ink rows the same way as the interface sample font.
    const auto inkRows = [](const QImage &image) {
        int top = image.height(), bottom = -1;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (qAlpha(image.pixel(x, y)) >= 128) {
                    top = qMin(top, y);
                    bottom = y;
                    break;
                }
        return bottom >= top ? bottom - top + 1 : 0;
    };
    const int reference = inkRows(renderPrivateText(QStringLiteral("Hox"), font, 0).image);
    font.setPixelSize(qBound(scale.minimum,
                             reference ? qRound(128.0 * scale.inkHeight / reference) : scale.base,
                             scale.maximum));
    QImage bitmap(size, QImage::Format_ARGB32_Premultiplied);
    if (bitmap.isNull())
        return {};
    bitmap.fill(Qt::transparent);
    QPainter painter(&bitmap);
    painter.setFont(font);
    painter.setPen(Qt::black);
    const auto draw = [&](QString line, QRect area) {
        const auto ranges = boldReading && !FT_IS_FIXED_WIDTH(face->handle)
                ? samplePrefixes(line)
                : QVector<QPair<int, int>>();
        line = elidePrivateText(line, font, area.width(), ranges);
        const auto rendered = renderPrivateText(line, font, boldReading ? 0 : weight, ranges);
        if (rendered.missingGlyphs)
            return false;
        drawOpticalPrivateText(painter, area, line, centered ? Qt::AlignHCenter : Qt::AlignLeft,
                               boldReading ? 0 : weight, ranges);
        return true;
    };
    int split = -1, widest = INT_MAX;
    if (wrap && measurePrivateText(text, font) > size.width())
        for (int i = 1; i < text.size() - 1; ++i)
            if (text[i].isSpace()) {
                const int width = qMax(measurePrivateText(text.left(i).trimmed(), font),
                                       measurePrivateText(text.mid(i + 1).trimmed(), font));
                if (width < widest) {
                    widest = width;
                    split = i;
                }
            }
    bool valid;
    const int lineHeight = split < 0
            ? 0
            : qMax(1, inkRows(renderPrivateText(text + QStringLiteral(" Hox"), font, 0).image))
                    + scale.lineGap;
    if (split >= 0 && lineHeight * 2 <= size.height()) {
        const int y = (size.height() - 2 * lineHeight) / 2;
        valid = draw(text.left(split).trimmed(), QRect(0, y, size.width(), lineHeight))
                && draw(text.mid(split + 1).trimmed(),
                        QRect(0, y + lineHeight, size.width(), lineHeight));
    } else
        valid = draw(text, QRect(QPoint(), size));
    painter.end();
    if (!valid)
        return {};
    // Keep only coverage. The GUI colors it for the current theme/selection.
    QImage mask(size, QImage::Format_Indexed8);
    QVector<QRgb> colors(256);
    for (int i = 0; i < 256; ++i)
        colors[i] = qRgba(0, 0, 0, i);
    mask.setColorTable(colors);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            mask.scanLine(y)[x] = qAlpha(bitmap.pixel(x, y));
    return mask;
}

int measurePrivateText(const QString &text, const QFont &font,
                       const QVector<QPair<int, int>> &boldRanges, PrivateTextEmphasis emphasis,
                       Qt::LayoutDirection direction)
{
    return layoutPrivateText(text, font, -1, boldRanges, false, {}, emphasis, direction).advance;
}

PrivateText measurePrivateTextLayout(const QString &text, const QFont &font,
                                     const QVector<QPair<int, int>> &boldRanges,
                                     PrivateTextEmphasis emphasis, Qt::LayoutDirection direction,
                                     const QVector<PrivateTextStyle> &styles)
{
    return layoutPrivateText(text, font, -1, boldRanges, false, {}, emphasis, direction, styles);
}

QSize measurePrivateTextSize(const QString &text, const QFont &font)
{
    const auto measured = layoutPrivateText(text, font, -1, {}, false);
    return QSize(measured.advance, measured.height);
}

int fittingTextEnd(const QVector<int> &ends, int start, int limit, int width,
                   const std::function<int(int end)> &measure, qreal &unitAdvance)
{
    const int first = int(std::upper_bound(ends.begin(), ends.end(), start) - ends.begin());
    const int last = int(std::upper_bound(ends.begin(), ends.end(), limit) - ends.begin()) - 1;
    if (first > last) {
        return start;
    }
    // The index whose end is closest to start + units without passing it.
    const auto indexAt = [&](qreal units) {
        const qreal target = qMin(qreal(limit), start + qMax(qreal(0), units));
        return int(std::upper_bound(ends.begin() + first, ends.begin() + last + 1, int(target))
                   - ends.begin())
                - 1;
    };
    // Without an earlier line, a first guess of 64 units is near a line of interface text.
    int guess = unitAdvance > 0 ? indexAt(width / unitAdvance) : indexAt(64);
    // fit is the furthest index known to fit and fail the nearest known not to.
    int fit = first - 1, fail = last + 1;
    for (int probes = 0; fail - fit > 1; ++probes) {
        // Uneven text can stall interpolation. Bisecting then bounds the search.
        if (probes >= 4) {
            guess = fit + (fail - fit) / 2;
        }
        guess = qBound(fit + 1, guess, fail - 1);
        const int units = ends[guess] - start;
        const int advance = measure(ends[guess]);
        if (advance > 0) {
            unitAdvance = qreal(advance) / units;
        }
        if (advance <= width) {
            fit = guess;
        } else {
            fail = guess;
        }
        guess = advance > 0 ? indexAt(units * qreal(width) / advance) : fit + (fail - fit) / 2;
    }
    return fit >= first ? ends[fit] : start;
}

QString elidePrivateText(const QString &text, const QFont &font, int width,
                         const QVector<QPair<int, int>> &boldRanges)
{
    const int advance = measurePrivateText(text, font, boldRanges);
    if (advance <= width) {
        return text;
    }
    const QString ellipsis(QChar(0x2026));
    if (measurePrivateText(ellipsis, font, boldRanges) > width) {
        return {};
    }
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
    QVector<int> boundaries;
    finder.toStart();
    for (int end = finder.toNextBoundary(); end >= 0; end = finder.toNextBoundary()) {
        boundaries.append(end);
    }
    qreal unitAdvance = qreal(advance) / qMax(1, int(text.size()));
    const int end = fittingTextEnd(
            boundaries, 0, text.size(), width,
            [&](int candidate) {
                return measurePrivateText(text.left(candidate) + ellipsis, font, boldRanges);
            },
            unitAdvance);
    return text.left(end) + ellipsis;
}

int interfaceLineAscent(const QFont &font)
{
    return fonts().library ? measurePrivateTextLayout(QStringLiteral("Mg"), font).lineAscent
                           : QFontMetrics(font).ascent();
}

QRect interfaceInkBounds(const QString &text, const QFont &font)
{
    if (!needsPrivateText(text, font))
        return QFontMetrics(font).tightBoundingRect(text);
    const auto rendered = renderPrivateText(text, font);
    QRect bounds;
    for (int y = 0; y < rendered.image.height(); ++y)
        for (int x = 0; x < rendered.image.width(); ++x)
            if (qAlpha(rendered.image.pixel(x, y)))
                bounds |= QRect(x, y, 1, 1);
    return bounds.translated(rendered.origin);
}

int interfaceLineHeight(const QFont &font)
{
    return fonts().library ? measurePrivateTextLayout(QStringLiteral("Mg"), font).lineHeight
                           : QFontMetrics(font).height();
}

int interfaceTextWidth(const QString &text, const QFont &font)
{
    if (!needsPrivateText(text, font)) {
#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
        return QFontMetrics(font).width(text);
#else
        return QFontMetrics(font).horizontalAdvance(text);
#endif
    }
    return measurePrivateText(text, font);
}

QString elideInterfaceText(const QString &text, const QFont &font, int width)
{
    if (!needsPrivateText(text, font)) {
        return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
    }
    return elidePrivateText(text, font, width);
}

namespace {
void drawPrivateText(QPainter &painter, const QRect &area, const QString &text,
                     Qt::Alignment alignment, int weight,
                     const QVector<QPair<int, int>> &boldRanges, bool optical,
                     bool shareBaseline = false)
{
    auto rendered = renderPrivateText(text, painter.font(), weight, boldRanges);
    if (rendered.image.isNull()) {
        return;
    }
    QPainter color(&rendered.image);
    color.setCompositionMode(QPainter::CompositionMode_SourceIn);
    color.fillRect(rendered.image.rect(), painter.pen().color());
    color.end();
    const int x = alignment.testFlag(Qt::AlignRight) ? area.right() + 1 - rendered.advance
            : alignment.testFlag(Qt::AlignHCenter)
            ? area.left() + (area.width() - rendered.advance) / 2
            : area.left();
    int y = alignment.testFlag(Qt::AlignBottom) ? area.bottom() + 1 - rendered.image.height()
            : alignment.testFlag(Qt::AlignVCenter)
            ? area.top() + (area.height() - rendered.image.height()) / 2
            : area.top();
    if (optical) {
        // Center the rows that hold ink, as Qt's tight bounds do for native
        // text, so a line without descenders does not sit high.
        int first = -1, last = -1;
        for (int row = 0; row < rendered.image.height(); ++row) {
            const auto *pixels = reinterpret_cast<const QRgb *>(rendered.image.constScanLine(row));
            for (int column = 0; column < rendered.image.width(); ++column)
                if (qAlpha(pixels[column])) {
                    if (first < 0)
                        first = row;
                    last = row;
                    break;
                }
        }
        if (first >= 0) {
            // Labels side by side share a baseline, so a descender must not
            // lift one of them. The image starts at origin.y() above the
            // baseline. Round letters overshoot it slightly; only a descender,
            // a tenth of the height above the baseline or more, is left out.
            const int baseline = -rendered.origin.y();
            const int overshoot = qMax(1, (baseline - first) / 10);
            if (shareBaseline && baseline > first && last >= baseline + overshoot)
                last = baseline - 1;
            y = qRound(area.top() + area.height() / 2.0 - (first + last + 1) / 2.0);
        }
    }
    painter.save();
    painter.setClipRect(area, Qt::IntersectClip);
    painter.drawImage(QPoint(x, y), rendered.image);
    painter.restore();
}
} // namespace

void drawInterfaceText(QPainter &painter, const QRect &area, const QString &text,
                       Qt::Alignment alignment, int weight,
                       const QVector<QPair<int, int>> &boldRanges)
{
    if (weight < 0 && !needsPrivateText(text, painter.font())) {
        painter.drawText(area, alignment, text);
        return;
    }
    drawPrivateText(painter, area, text, alignment, weight, boldRanges, false);
}

void drawOpticalPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment, int weight,
                            const QVector<QPair<int, int>> &boldRanges)
{
    drawPrivateText(painter, area, text, alignment, weight, boldRanges, true);
}

void drawSegmentPrivateText(QPainter &painter, const QRect &area, const QString &text,
                            Qt::Alignment alignment)
{
    drawPrivateText(painter, area, text, alignment, -1, {}, true, true);
}

int PrivateTextLabel::textWidth(const QString &value) const
{
    return interfaceTextWidth(value, font());
}

QString PrivateTextLabel::elidedText(const QString &value, int width) const
{
    return elideInterfaceText(value, font(), width);
}

QSize PrivateTextLabel::sizeHint() const
{
    return needsPrivateText(text(), font()) ? measurePrivateTextSize(text(), font())
                                            : QLabel::sizeHint();
}

QSize PrivateTextLabel::minimumSizeHint() const
{
    return needsPrivateText(text(), font()) ? QSize() : QLabel::minimumSizeHint();
}

void PrivateTextLabel::paintEvent(QPaintEvent *event)
{
    if (!needsPrivateText(text(), font()) && !(wordWrap() && maximumLines > 0)) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    const auto visual = QStyle::visualAlignment(
            text().isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight, alignment());
    if (wordWrap()) {
        QVector<QRect> rectangles;
        drawWrappedInterfaceText(painter, rect(), text(), visual, maximumLines, lineSpacing,
                                 highlightStyle.isEmpty() ? nullptr : &rectangles);
        QRegion region;
        for (const auto &rectangle : rectangles)
            region += rectangle;
        if (!highlightStyle.isEmpty())
            render::paintHighlightMark(painter, rectangles, region, highlightColor, highlightStyle,
                                       darkTheme(this));
    } else
        drawInterfaceText(painter, rect(), text(), visual);
}
} // namespace kobalt::ui
