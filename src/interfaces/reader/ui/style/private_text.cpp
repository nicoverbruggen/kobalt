#include "private_text.h"

#include "fonts.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"

#include <QCache>
#include <QFontMetrics>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPainter>
#include <QStyle>
#include <QTextBoundaryFinder>
#include <QVector>

#include <algorithm>
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
struct Face {
    QByteArray bytes;
    FT_Face handle = nullptr;
    ~Face() {
        if (handle) {
            FT_Done_Face(handle);
        }
    }
};
struct RegisteredFace {
    QString file;
    int index;
    bool bold, italic, privateOnly;
};
struct Fonts {
    FT_Library library = nullptr;
    QMap<QString, QByteArray> sources;
    QMap<QString, std::function<QByteArray()>> pending;
    QMap<QString, std::shared_ptr<Face>> faces;
    QCache<QString, PrivateText> layouts{2048};
    QMap<QString, QVector<RegisteredFace>> registered;
    QMap<QString, QString> registeredPaths;
    QMap<QString, PrivateFontRegistration> registrations;
    ~Fonts() {
        faces.clear();
        if (library) {
            FT_Done_FreeType(library);
        }
    }
    FT_Face face(const QString &file, int index = 0) {
        const QString key = file + ':' + QString::number(index);
        auto found = faces.find(key);
        if (found != faces.end()) {
            return found.value()->handle;
        }
        if (!sources.contains(file)) {
            const auto read = pending.value(file);
            const auto bytes = read ? read() : QByteArray();
            if (bytes.isEmpty())
                return nullptr;
            sources.insert(file, bytes);
        }
        auto value = std::make_shared<Face>();
        value->bytes = sources.value(file);
        if (value->bytes.isEmpty() ||
            FT_New_Memory_Face(library, reinterpret_cast<const FT_Byte *>(value->bytes.constData()),
                               value->bytes.size(), index, &value->handle)) {
            return nullptr;
        }
        faces.insert(key, value);
        return value->handle;
    }
};
Fonts &fonts() {
    static Fonts value;
    return value;
}
QString primaryFile(const QFont &font) {
    const bool bold = font.weight() >= QFont::Bold;
    if (font.family().contains("Mono")) {
        return bold ? "IBMPlexMono-Bold.ttf" : "IBMPlexMono-Regular.ttf";
    }
    if (font.family().contains("Serif") && !font.family().contains("Sans")) {
        return font.italic() ? "InstrumentSerif-Italic.ttf" : "InstrumentSerif-Regular.ttf";
    }
    return font.italic() ? "IBMPlexSans-Italic.ttf" : bold ? "IBMPlexSans-Bold.ttf" : "IBMPlexSans-Regular.ttf";
}
bool interfaceFamily(const QFont &font) {
    return font.family() == "Kobalt UI Sans" || font.family() == "Kobalt UI Mono" || font.family() == "Kobalt UI Serif";
}
const RegisteredFace *registeredFace(const QFont &font) {
    const auto found = fonts().registered.constFind(font.family());
    if (found == fonts().registered.cend() || found.value().isEmpty()) {
        return nullptr;
    }
    const RegisteredFace *best = &found.value().first();
    int bestScore = -1;
    for (const auto &face : found.value()) {
        const int score =
            (face.bold == (font.weight() >= QFont::Bold) ? 1 : 0) + (face.italic == font.italic() ? 2 : 0);
        if (score > bestScore) {
            best = &face;
            bestScore = score;
        }
    }
    return best;
}
FT_Face primaryFace(const QFont &font) {
    if (interfaceFamily(font)) {
        return fonts().face(primaryFile(font));
    }
    if (const auto *registered = registeredFace(font)) {
        return fonts().face(registered->file, registered->index);
    }
    return nullptr;
}
FT_Face fallbackFace(const QString &regular, const QFont &font, int index = 0) {
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
            if (fonts().pending.contains(file) || fonts().sources.contains(file)) {
                if (const auto face = fonts().face(file, index)) {
                    return face;
                }
            }
        }
    }
    return fonts().face(regular, index);
}
FT_Face faceFor(uint character, QChar::Script script, FT_Face primary, const QFont &font) {
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
    for (const auto &file : {QStringLiteral("NotoSerif-Regular.ttf"), QStringLiteral("NotoSansMath-Regular.ttf")}) {
        const auto face = fallbackFace(file, font);
        if (face && FT_Get_Char_Index(face, character)) {
            return face;
        }
    }
    return primary;
}
struct Run {
    int first, count;
    FT_Face face;
    QChar::Script script;
    FriBidiLevel level;
    int visual;
};
struct Glyph {
    QImage mask;
    QPoint position;
};
}
bool initializePrivateInterfaceText(const AssetBundle &assets) {
    QMap<QString, std::function<QByteArray()>> pending;
    QStringList files{"IBMPlexSans-Regular.ttf",    "IBMPlexSans-Bold.ttf",     "IBMPlexSans-Italic.ttf",
                      "IBMPlexMono-Regular.ttf",    "IBMPlexMono-Bold.ttf",     "InstrumentSerif-Regular.ttf",
                      "InstrumentSerif-Italic.ttf", "NotoSansMath-Regular.ttf", "NotoSerifCJK-Regular.ttc"};
    for (const auto *file : serifFallbackFontFiles) {
        files.append(QLatin1String(file));
    }
    for (const auto *file : scriptFallbackFontFiles) {
        files.append(QLatin1String(file));
    }
    for (const auto &file : files) {
        const auto read = assets.deferredFontData(file);
        if (!read) {
            return false;
        }
        pending.insert(file, read);
    }
    auto &state = fonts();
    if (!state.library && FT_Init_FreeType(&state.library)) {
        return false;
    }
    state.layouts.clear();
    state.faces.clear();
    state.registered.clear();
    state.registeredPaths.clear();
    state.registrations.clear();
    state.sources.clear();
    state.pending = pending;
    return true;
}
qint64 privateInterfaceFontBytes() {
    qint64 bytes = 0;
    for (const auto &source : fonts().sources)
        bytes += source.size();
    return bytes;
}
PrivateFontRegistration registerPrivateInterfaceFont(const QString &path, const QString &expectedFamily,
                                                     bool forcePrivate) {
    auto &state = fonts();
    if (!state.library) {
        return PrivateFontRegistration::Failed;
    }
    const QString registrationKey = path + '\n' + expectedFamily;
    QString key = state.registeredPaths.value(path);
    if (forcePrivate && !key.isEmpty()) {
        bool changed = false;
        for (auto &entries : state.registered) {
            for (auto &entry : entries) {
                if (entry.file == key && !entry.privateOnly) {
                    entry.privateOnly = true;
                    changed = true;
                }
            }
        }
        if (changed) {
            state.registrations.clear();
            state.layouts.clear();
        }
    }
    const auto cached = state.registrations.constFind(registrationKey);
    if (cached != state.registrations.cend()) {
        return cached.value();
    }
    if (key.isEmpty()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 64 * 1024 * 1024) {
            return PrivateFontRegistration::Failed;
        }
        const auto bytes = file.readAll();
        if (bytes.isEmpty()) {
            return PrivateFontRegistration::Failed;
        }
        const QString bundled = QFileInfo(path).fileName();
        if (!state.sources.contains(bundled) && state.pending.contains(bundled))
            state.face(bundled);
        if (state.sources.contains(bundled) && state.sources.value(bundled) == bytes) {
            key = bundled;
        } else {
            key = "reading:" + path;
            state.sources.insert(key, bytes);
        }
        state.registeredPaths.insert(path, key);
    }
    if (!expectedFamily.isEmpty() && state.registered.contains(expectedFamily)) {
        for (const auto &registered : state.registered.value(expectedFamily)) {
            if (registered.file == key) {
                const auto result =
                    registered.privateOnly ? PrivateFontRegistration::Private : PrivateFontRegistration::Native;
                state.registrations.insert(registrationKey, result);
                return result;
            }
        }
    }
    const auto first = state.face(key);
    if (!first || first->num_faces <= 0 || first->num_faces > 256) {
        return PrivateFontRegistration::Failed;
    }
    bool privateOnly = forcePrivate || first->num_faces > 1;
    QVector<QPair<QString, RegisteredFace>> pending;
    for (int index = 0; index < first->num_faces; ++index) {
        const auto face = state.face(key, index);
        if (!face) {
            return PrivateFontRegistration::Failed;
        }
        const bool unsafe = forcePrivate || first->num_faces > 1 || QByteArray(FT_Get_Font_Format(face)) != "TrueType";
        privateOnly = privateOnly || unsafe;
        const QString family = QString::fromUtf8(face->family_name);
        const RegisteredFace entry{key, index, bool(face->style_flags & FT_STYLE_FLAG_BOLD),
                                   bool(face->style_flags & FT_STYLE_FLAG_ITALIC), unsafe};
        pending.append({family, entry});
        if (!expectedFamily.isEmpty() && expectedFamily != family && first->num_faces == 1) {
            pending.append({expectedFamily, entry});
        }
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
    state.layouts.clear();
    const auto result = privateOnly ? PrivateFontRegistration::Private : PrivateFontRegistration::Native;
    state.registrations.insert(registrationKey, result);
    return result;
}
bool needsPrivateText(const QString &text, const QFont &font) {
    if (!fonts().library) {
        return false;
    }
    if (const auto *registered = registeredFace(font)) {
        if (registered->privateOnly) {
            return true;
        }
    }
    const auto primary = primaryFace(font);
    if (!primary) {
        return false;
    }
    for (const auto character : text.toUcs4()) {
        if (QChar::category(character) != QChar::Other_Format && !FT_Get_Char_Index(primary, character)) {
            return true;
        }
    }
    return false;
}
static PrivateText layoutPrivateText(const QString &input, const QFont &font, int weight,
                                     const QVector<QPair<int, int>> &boldRanges, bool rasterize,
                                     const QVector<QPair<int, int>> &underlineRanges = {},
                                     PrivateTextEmphasis emphasis = PrivateTextEmphasis::Synthetic,
                                     Qt::LayoutDirection direction = Qt::LayoutDirectionAuto) {
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
    const int steps = weight < 0 ? (interfaceFamily(font) ? interfaceWeight() : 0) : qBound(0, weight, 12);
    QString cacheKey = font.toString() + ':' + QString::number(steps) + ':' + QString::number(rasterize) + ':' +
                       QString::number(font.capitalization()) + ':' + QString::number(font.letterSpacingType()) + ':' +
                       QString::number(font.letterSpacing(), 'g', 16) + ':' +
                       QString::number(font.wordSpacing(), 'g', 16) + ':' + QString::number(font.stretch()) + ':' +
                       QString::number(font.kerning());
    for (const auto &range : boldRanges) {
        cacheKey += ':' + QString::number(range.first) + ',' + QString::number(range.second);
    }
    cacheKey += ":underline:" + QString::number(int(emphasis)) + ':' + QString::number(int(direction));
    for (const auto &range : underlineRanges) {
        cacheKey += ':' + QString::number(range.first) + ',' + QString::number(range.second);
    }
    cacheKey += ':' + text;
    if (const auto *cached = fonts().layouts.object(cacheKey)) {
        return *cached;
    }
    const auto characters = text.toUcs4();
    const int count = characters.size();
    QVector<int> utf16Offsets;
    int utf16 = 0;
    for (const auto character : characters) {
        utf16Offsets.append(utf16);
        utf16 += character > 0xffff ? 2 : 1;
    }
    QVector<FriBidiLevel> levels(count);
    QVector<FriBidiStrIndex> logicalToVisual(count);
    FriBidiParType base = direction == Qt::RightToLeft   ? FRIBIDI_PAR_RTL
                          : direction == Qt::LeftToRight ? FRIBIDI_PAR_LTR
                                                         : FRIBIDI_PAR_ON;
    if (!fribidi_log2vis(characters.constData(), count, &base, nullptr, logicalToVisual.data(), nullptr,
                         levels.data())) {
        return result;
    }
    std::vector<Run> runs;
    const auto primary = primaryFace(font);
    QFont boldFont = font;
    boldFont.setWeight(QFont::Bold);
    const auto boldPrimary = emphasis == PrivateTextEmphasis::FontFace ? primaryFace(boldFont) : primary;
    QVector<FT_Face> characterFaces(count, nullptr);
    QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, text);
    boundaries.toStart();
    int firstCharacter = 0;
    for (int start = 0, end = boundaries.toNextBoundary(); end >= 0; start = end, end = boundaries.toNextBoundary()) {
        const auto cluster = text.mid(start, end - start).toUcs4();
        if (cluster.isEmpty()) {
            continue;
        }
        auto covers = [&](FT_Face face) {
            if (!face) {
                return false;
            }
            for (const auto character : cluster) {
                if (QChar::category(character) != QChar::Other_Format && !FT_Get_Char_Index(face, character)) {
                    return false;
                }
            }
            return true;
        };
        FT_Face requested = primary;
        const QFont *requestedFont = &font;
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
        if (!covers(face)) {
            face = faceFor(cluster.first(), QChar::script(cluster.first()), requested, *requestedFont);
            if (!covers(face)) {
                QStringList files = fonts().pending.keys();
                for (const auto &file : fonts().sources.keys())
                    if (!files.contains(file))
                        files.append(file);
                for (const auto &file : files) {
                    if (file.endsWith("-Bold.ttf") || file.endsWith("-Italic.ttf") ||
                        file.endsWith("-BoldItalic.ttf")) {
                        const auto regular = file.left(file.lastIndexOf('-') + 1) + "Regular.ttf";
                        if (fonts().pending.contains(regular) || fonts().sources.contains(regular)) {
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
        for (int i = 0; i < cluster.size(); ++i) {
            characterFaces[firstCharacter++] = face;
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
        if (!runs.empty() && runs.back().face == face && runs.back().script == script &&
            runs.back().level == levels[i]) {
            ++runs.back().count;
            runs.back().visual = qMin(runs.back().visual, int(logicalToVisual[i]));
        } else {
            runs.push_back({i, 1, face, script, levels[i], int(logicalToVisual[i])});
        }
    }
    std::sort(runs.begin(), runs.end(), [](const Run &a, const Run &b) {
        return a.visual < b.visual;
    });
    const int pixels = font.pixelSize() > 0 ? font.pixelSize() : 32;
    QVector<Glyph> glyphs;
    QVector<QRect> underlines;
    QRect bounds;
    qint64 pen = 0;
    int maximumLineDescent = 0;
    uint previousCluster = uint(-1);
    for (const auto &run : runs) {
        if (FT_Set_Pixel_Sizes(run.face, qMax(1, pixels * (font.stretch() > 0 ? font.stretch() : 100) / 100), pixels)) {
            return {};
        }
        result.height =
            qMax(result.height, int((run.face->size->metrics.ascender - run.face->size->metrics.descender + 63) / 64));
        int lineAscent = int((run.face->size->metrics.ascender + 63) / 64);
        int lineDescent = int((-run.face->size->metrics.descender + 63) / 64);
        // Older Qt uses the horizontal header for document line boxes. Keep
        // excerpt baselines and spacing when FreeType selects OS/2 typo metrics.
        const auto *header = static_cast<const TT_HoriHeader *>(FT_Get_Sfnt_Table(run.face, ft_sfnt_hhea));
        if (header && header->Ascender > 0 && header->Descender <= 0) {
            lineAscent = int((FT_MulFix(header->Ascender, run.face->size->metrics.y_scale) + 63) / 64);
            lineDescent = int((-FT_MulFix(header->Descender, run.face->size->metrics.y_scale) + 63) / 64);
        }
        result.lineAscent = qMax(result.lineAscent, lineAscent);
        maximumLineDescent = qMax(maximumLineDescent, lineDescent);
        result.lineHeight = result.lineAscent + maximumLineDescent;
        hb_font_t *hbFont = hb_ft_font_create_referenced(run.face);
        hb_ft_font_set_load_flags(hbFont, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
        hb_buffer_t *buffer = hb_buffer_create();
        hb_buffer_add_utf32(buffer, characters.constData(), count, run.first, run.count);
        hb_buffer_guess_segment_properties(buffer);
        hb_buffer_set_direction(buffer, run.level & 1 ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_feature_t features[3];
        hb_feature_from_string(font.kerning() ? "kern=1" : "kern=0", -1, &features[0]);
        const bool separateLetters = !boldRanges.isEmpty() || !underlineRanges.isEmpty() ||
                                     (font.letterSpacingType() == QFont::AbsoluteSpacing && font.letterSpacing() != 0);
        if (separateLetters) {
            hb_feature_from_string("liga=0", -1, &features[1]);
            hb_feature_from_string("clig=0", -1, &features[2]);
        }
        hb_shape(hbFont, buffer, features, separateLetters ? 3 : 1);
        unsigned glyphCount = 0;
        const auto *info = hb_buffer_get_glyph_infos(buffer, &glyphCount);
        const auto *positions = hb_buffer_get_glyph_positions(buffer, &glyphCount);
        for (unsigned i = 0; i < glyphCount; ++i) {
            if (previousCluster != uint(-1) && previousCluster != info[i].cluster &&
                font.letterSpacingType() == QFont::AbsoluteSpacing) {
                pen += qRound64(font.letterSpacing() * 64);
            }
            previousCluster = info[i].cluster;
            bool bold = false;
            for (const auto &range : boldRanges) {
                if (utf16Offsets[int(info[i].cluster)] >= range.first &&
                    utf16Offsets[int(info[i].cluster)] < range.first + range.second) {
                    bold = true;
                    break;
                }
            }
            if (emphasis == PrivateTextEmphasis::FontFace && (run.face->style_flags & FT_STYLE_FLAG_BOLD)) {
                bold = false;
            }
            if (!info[i].codepoint) {
                ++result.missingGlyphs;
            }
            if (rasterize) {
                for (const auto &range : underlineRanges) {
                    if (utf16Offsets[int(info[i].cluster)] >= range.first &&
                        utf16Offsets[int(info[i].cluster)] < range.first + range.second) {
                        const int position = qMax(
                            1, int(-FT_MulFix(run.face->underline_position, run.face->size->metrics.y_scale) / 64));
                        const int thickness = qMax(
                            1,
                            int((FT_MulFix(run.face->underline_thickness, run.face->size->metrics.y_scale) + 63) / 64));
                        const QRect underline(int(pen / 64), position, qMax(1, int((positions[i].x_advance + 63) / 64)),
                                              thickness);
                        bounds = bounds.united(underline);
                        underlines.append(underline);
                        break;
                    }
                }
            }
            if (rasterize && pen < 16384 * 64 &&
                !FT_Load_Glyph(run.face, info[i].codepoint, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) {
                if ((steps || bold) && run.face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
                    const FT_Pos strength =
                        bold ? qMax(pixels * 64 * steps / 256, pixels * 64 / 24) : pixels * 64 * steps / 256;
                    FT_Outline_Embolden(&run.face->glyph->outline, strength);
                    FT_Outline_Translate(&run.face->glyph->outline, -strength / 2, -strength / 2);
                }
                if (!FT_Render_Glyph(run.face->glyph, FT_RENDER_MODE_NORMAL)) {
                    const auto &bitmap = run.face->glyph->bitmap;
                    if (bitmap.width && bitmap.rows &&
                        (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY || bitmap.num_grays != 256 || !bitmap.buffer ||
                         qAbs(qint64(bitmap.pitch)) < bitmap.width || bitmap.width > 16384 || bitmap.rows > 4096 ||
                         qint64(bitmap.width) * bitmap.rows > 8 * 1024 * 1024)) {
                        ++result.missingGlyphs;
                    } else if (bitmap.width && bitmap.rows) {
                        QImage mask(bitmap.width, bitmap.rows, QImage::Format_ARGB32_Premultiplied);
                        if (mask.isNull()) {
                            ++result.missingGlyphs;
                        }
                        mask.fill(Qt::transparent);
                        for (unsigned y = 0; y < bitmap.rows && !mask.isNull(); ++y) {
                            auto *line = reinterpret_cast<QRgb *>(mask.scanLine(y));
                            const auto *source = bitmap.buffer + qint64(y) * bitmap.pitch;
                            for (unsigned x = 0; x < bitmap.width; ++x) {
                                line[x] = qRgba(0, 0, 0, source[x]);
                            }
                        }
                        const QPoint origin((pen + positions[i].x_offset) / 64 + run.face->glyph->bitmap_left,
                                            -positions[i].y_offset / 64 - run.face->glyph->bitmap_top);
                        bounds = bounds.united(QRect(origin, mask.size()));
                        glyphs.append({mask, origin});
                    }
                } else {
                    ++result.missingGlyphs;
                }
            } else if (rasterize && pen < 16384 * 64) {
                ++result.missingGlyphs;
            }
            pen += font.letterSpacingType() == QFont::PercentageSpacing
                       ? qRound64(positions[i].x_advance * (font.letterSpacing() == 0 ? 100 : font.letterSpacing()) /
                                  100.0)
                       : positions[i].x_advance;
            if (info[i].cluster < uint(count) && QChar::isSpace(characters[int(info[i].cluster)])) {
                pen += qRound64(font.wordSpacing() * 64);
            }
        }
        hb_buffer_destroy(buffer);
        hb_font_destroy(hbFont);
    }
    result.advance = int(qMin<qint64>(2147483647, qMax<qint64>(0, (pen + 63) / 64)));
    if (!rasterize) {
        const int cost = qMax(1, (cacheKey.size() * 2 + 1023) / 1024);
        if (cost <= fonts().layouts.maxCost()) {
            fonts().layouts.insert(cacheKey, new PrivateText(result), cost);
        }
        return result;
    }
    bounds = bounds.united(QRect(0, -pixels, result.advance, pixels));
    if (bounds.isEmpty() || bounds.width() > 16384 || bounds.height() > 4096 ||
        qint64(bounds.width()) * bounds.height() > 8 * 1024 * 1024) {
        return result;
    }
    result.image = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
    result.origin = bounds.topLeft();
    result.image.fill(Qt::transparent);
    QPainter painter(&result.image);
    for (const auto &glyph : glyphs) {
        painter.drawImage(glyph.position - bounds.topLeft(), glyph.mask);
    }
    for (const auto &underline : underlines) {
        painter.fillRect(underline.translated(-bounds.topLeft()), Qt::black);
    }
    painter.end();
    const int cost = qMax(1, (result.image.bytesPerLine() * result.image.height() + cacheKey.size() * 2 + 1023) / 1024);
    if (cost <= fonts().layouts.maxCost()) {
        fonts().layouts.insert(cacheKey, new PrivateText(result), cost);
    }
    return result;
}
PrivateText renderPrivateText(const QString &text, const QFont &font, int weight,
                              const QVector<QPair<int, int>> &boldRanges,
                              const QVector<QPair<int, int>> &underlineRanges, PrivateTextEmphasis emphasis,
                              Qt::LayoutDirection direction) {
    return layoutPrivateText(text, font, weight, boldRanges, true, underlineRanges, emphasis, direction);
}
int measurePrivateText(const QString &text, const QFont &font, const QVector<QPair<int, int>> &boldRanges,
                       PrivateTextEmphasis emphasis, Qt::LayoutDirection direction) {
    return layoutPrivateText(text, font, -1, boldRanges, false, {}, emphasis, direction).advance;
}
PrivateText measurePrivateTextLayout(const QString &text, const QFont &font, const QVector<QPair<int, int>> &boldRanges,
                                     PrivateTextEmphasis emphasis, Qt::LayoutDirection direction) {
    return layoutPrivateText(text, font, -1, boldRanges, false, {}, emphasis, direction);
}
QSize measurePrivateTextSize(const QString &text, const QFont &font) {
    const auto measured = layoutPrivateText(text, font, -1, {}, false);
    return QSize(measured.advance, measured.height);
}
QString elidePrivateText(const QString &text, const QFont &font, int width,
                         const QVector<QPair<int, int>> &boldRanges) {
    if (measurePrivateText(text, font, boldRanges) <= width) {
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
    QString best = ellipsis;
    int first = 0, last = boundaries.size() - 1;
    while (first <= last) {
        const int middle = first + (last - first) / 2;
        const QString candidate = text.left(boundaries[middle]) + ellipsis;
        if (measurePrivateText(candidate, font, boldRanges) <= width) {
            best = candidate;
            first = middle + 1;
        } else {
            last = middle - 1;
        }
    }
    return best;
}
int interfaceTextWidth(const QString &text, const QFont &font) {
    if (!needsPrivateText(text, font)) {
#if QT_VERSION < QT_VERSION_CHECK(5, 11, 0)
        return QFontMetrics(font).width(text);
#else
        return QFontMetrics(font).horizontalAdvance(text);
#endif
    }
    return measurePrivateText(text, font);
}
QString elideInterfaceText(const QString &text, const QFont &font, int width) {
    if (!needsPrivateText(text, font)) {
        return QFontMetrics(font).elidedText(text, Qt::ElideRight, width);
    }
    return elidePrivateText(text, font, width);
}
namespace {
void drawPrivateText(QPainter &painter, const QRect &area, const QString &text, Qt::Alignment alignment, int weight,
                     const QVector<QPair<int, int>> &boldRanges, bool optical) {
    auto rendered = renderPrivateText(text, painter.font(), weight, boldRanges);
    if (rendered.image.isNull()) {
        return;
    }
    QPainter color(&rendered.image);
    color.setCompositionMode(QPainter::CompositionMode_SourceIn);
    color.fillRect(rendered.image.rect(), painter.pen().color());
    color.end();
    const int x = alignment.testFlag(Qt::AlignRight)     ? area.right() + 1 - rendered.advance
                  : alignment.testFlag(Qt::AlignHCenter) ? area.left() + (area.width() - rendered.advance) / 2
                                                         : area.left();
    int y = alignment.testFlag(Qt::AlignBottom)    ? area.bottom() + 1 - rendered.image.height()
            : alignment.testFlag(Qt::AlignVCenter) ? area.top() + (area.height() - rendered.image.height()) / 2
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
        if (first >= 0)
            y = qRound(area.top() + area.height() / 2.0 - (first + last + 1) / 2.0);
    }
    painter.save();
    painter.setClipRect(area, Qt::IntersectClip);
    painter.drawImage(QPoint(x, y), rendered.image);
    painter.restore();
}
}
void drawInterfaceText(QPainter &painter, const QRect &area, const QString &text, Qt::Alignment alignment, int weight,
                       const QVector<QPair<int, int>> &boldRanges) {
    if (!needsPrivateText(text, painter.font())) {
        painter.drawText(area, alignment, text);
        return;
    }
    drawPrivateText(painter, area, text, alignment, weight, boldRanges, false);
}
void drawOpticalPrivateText(QPainter &painter, const QRect &area, const QString &text, Qt::Alignment alignment,
                            int weight, const QVector<QPair<int, int>> &boldRanges) {
    drawPrivateText(painter, area, text, alignment, weight, boldRanges, true);
}
int PrivateTextLabel::textWidth(const QString &value) const {
    return interfaceTextWidth(value, font());
}
QString PrivateTextLabel::elidedText(const QString &value, int width) const {
    return elideInterfaceText(value, font(), width);
}
QSize PrivateTextLabel::sizeHint() const {
    return needsPrivateText(text(), font()) ? measurePrivateTextSize(text(), font()) : QLabel::sizeHint();
}
QSize PrivateTextLabel::minimumSizeHint() const {
    return needsPrivateText(text(), font()) ? QSize() : QLabel::minimumSizeHint();
}
void PrivateTextLabel::paintEvent(QPaintEvent *event) {
    if (!needsPrivateText(text(), font())) {
        QLabel::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setFont(font());
    painter.setPen(palette().color(QPalette::WindowText));
    const auto visual =
        QStyle::visualAlignment(text().isRightToLeft() ? Qt::RightToLeft : Qt::LeftToRight, alignment());
    drawInterfaceText(painter, rect(), text(), visual);
}
}
