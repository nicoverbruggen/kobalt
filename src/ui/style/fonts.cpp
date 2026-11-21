#include "fonts.h"
#include "private_text.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QRawFont>
#include "support/diagnostics.h"
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <QMap>
#include <functional>

namespace kobalt::ui {
struct OpeningFontData
{
    quint64 generation;
    QMap<QString, QByteArray> sources, digests;
};

namespace {
struct Face
{
    const char *file, *style;
};

struct Family
{
    const char *name;
    QVector<Face> faces;
};
// The interface registers the bundled faces unchanged, so Qt draws its few
// remaining controls, such as editable fields, in the regular weight. The
// private renderer draws everything else and adds the interface weight itself.
enum class FamilyIndex { Sans, Mono, Serif };

const QVector<Family> &families()
{
    static const QVector<Family> value{
        { "IBM Plex Sans",
          { { "IBMPlexSans-Regular.ttf", "Regular" },
            { "IBMPlexSans-Bold.ttf", "Bold" },
            { "IBMPlexSans-Italic.ttf", "Italic" } } },
        { "IBM Plex Mono",
          { { "IBMPlexMono-Regular.ttf", "Regular" }, { "IBMPlexMono-Bold.ttf", "Bold" } } },
        { "Instrument Serif",
          { { "InstrumentSerif-Regular.ttf", "Regular" },
            { "InstrumentSerif-Italic.ttf", "Italic" } } },
    };
    return value;
}

struct Registration
{
    bool registered = false;
    unsigned fallbackGeneration = 0;
    QVector<QVector<int>> ids;
    QVector<int> scriptFallbackIds;
    // Interface faces and script fallbacks load separately: the opening view
    // needs only the faces, before the whole asset bundle is verified.
    OpeningFonts active, prepared;
    QMap<QString, QByteArray> fallbackSources;
    QMap<QString, std::function<QByteArray()>> fallbackReaders;
};

Registration &registration()
{
    static Registration value;
    return value;
}

int requestedSteps = 0;
int stripAddedWeight = 2;
QString selectedInterfaceFamily, selectedInterfaceMonoFamily, selectedHeaderFooterFamily;

int faceCount(const QVector<QVector<int>> &ids)
{
    int count = 0;
    for (const auto &family : ids)
        count += family.size();
    return count;
}

void removeFaces(const QVector<QVector<int>> &ids)
{
    for (const auto &family : ids)
        for (const int id : family)
            QFontDatabase::removeApplicationFont(id);
}

QVector<QVector<int>> addFaces(const OpeningFontData &faces)
{
    QVector<QVector<int>> ids;
    for (const auto &family : families()) {
        ids.append(QVector<int>());
        // Qt 5.2 files a face under the first face registered for its weight
        // and slant, so the regular face comes first.
        for (const auto &face : family.faces) {
            const auto source = faces.sources.value(face.file);
            const int id =
                    source.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(source);
            if (id >= 0)
                ids.last().append(id);
        }
    }
    return ids;
}

// The faces register on first use, so a reader that never draws with Qt
// fonts never adds them.
void registerFaces()
{
    auto &current = registration();
    if (current.registered || !current.active)
        return;
    current.ids = addFaces(*current.active);
    current.registered = true;
}

QString family(FamilyIndex index, const QString &missing = {})
{
    registerFaces();
    return registration().ids.value(int(index)).isEmpty()
            ? missing
            : QString::fromLatin1(families()[int(index)].name);
}

QFont sizedFont(const QString &family, int pixels)
{
    QFont font(family);
    // Draw unhinted, as the private FreeType path does, so both paths keep
    // the letters' shapes and widths. At 212 ppi and above, hinting distorts
    // more than it sharpens on e-paper.
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setWeight(QFont::Normal);
    // Set every face attribute, so a painter or widget never fills one in
    // from the widget font. Nickel's style sheet makes check boxes italic.
    font.setStyle(QFont::StyleNormal);
    font.setUnderline(false);
    font.setOverline(false);
    font.setStrikeOut(false);
    if (pixels > 0)
        font.setPixelSize(pixels);
    return font;
}
} // namespace

namespace {
// Replaces the registered faces when their bytes change. Keeps the faces in
// use when the new set cannot be registered in full.
bool installFaces(const AssetBundle &assets, const OpeningFonts &faces, bool opening, bool *changed)
{
    auto &current = registration();
    const bool different = !current.active
            || (current.active != faces && current.active->digests != faces->digests);
    QVector<QVector<int>> added;
    if (different && current.registered) {
        added = addFaces(*faces);
        if (faceCount(added) != faces->sources.size()) {
            removeFaces(added);
            return false;
        }
    }
    if (!initializePrivateInterfaceText(assets, opening, faces->sources)) {
        removeFaces(added);
        return false;
    }
    if (!added.isEmpty()) {
        removeFaces(current.ids);
        current.ids = std::move(added);
    }
    *changed = different;
    current.active = faces;
    current.prepared.reset();
    return true;
}

QStringList interfaceFiles()
{
    QStringList files;
    for (const auto &family : families())
        for (const auto &face : family.faces)
            files.append(QLatin1String(face.file));
    return files;
}

OpeningFonts readFaces(const AssetBundle &assets, bool opening,
                       const std::function<bool()> &current = {})
{
    auto faces = std::make_shared<OpeningFontData>();
    faces->generation = assets.generation();
    const auto files = interfaceFiles();
    if (opening) {
        for (const auto &file : files) {
            QByteArray digest;
            const auto bytes = assets.manifestFontData(file, &digest);
            if (bytes.isEmpty())
                return {};
            faces->sources.insert(file, bytes);
            faces->digests.insert(file, digest);
        }
    } else {
        const auto readers = assets.deferredFontData(files, &faces->digests);
        if (readers.size() != files.size())
            return {};
        for (auto it = readers.cbegin(); it != readers.cend(); ++it) {
            if (current && !current())
                return {};
            const auto bytes = it.value()();
            if (bytes.isEmpty())
                return {};
            faces->sources.insert(it.key(), bytes);
        }
    }
    return faces->generation == assets.generation() ? faces : OpeningFonts();
}

// The faces of the bundle's current verification that need no file read.
OpeningFonts preparedFaces(const AssetBundle &assets)
{
    const auto generation = assets.generation();
    const auto &current = registration();
    if (current.prepared && current.prepared->generation == generation)
        return current.prepared;
    if (current.active && current.active->generation == generation)
        return current.active;
    return {};
}

OpeningFonts interfaceFaces(const AssetBundle &assets, bool opening)
{
    const bool explicitBundle = assets.verification() == AssetBundle::Verification::Explicit;
    // On-demand opening screens can draw before unrelated assets are verified.
    const bool requireVerified = explicitBundle || !opening;
    if (requireVerified && !assets.available())
        return {};
    const auto generation = assets.generation();
    const auto &current = registration();
    // Explicit bundles change generation with every verification. Their worker
    // read against that generation's digests before it published these bytes.
    if (explicitBundle)
        return preparedFaces(assets);
    const auto verified = assets.verifiedFontDigests();
    const auto matches = [&](const OpeningFonts &faces) {
        if (!faces || faces->generation != generation)
            return false;
        // Before the first full check, this loading view can reuse its own
        // manifest-checked bytes until the bundle is invalidated.
        if (opening && verified.isEmpty())
            return true;
        for (auto it = faces->digests.cbegin(); it != faces->digests.cend(); ++it)
            if (verified.value(assets.file("fonts/" + it.key())) != it.value())
                return false;
        return true;
    };
    if (matches(current.prepared))
        return current.prepared;
    if (matches(current.active))
        return current.active;
    return readFaces(assets, opening);
}
} // namespace

OpeningFonts readOpeningFonts(const AssetBundle &assets, const std::function<bool()> &current)
{
    if ((current && !current()) || !assets.available())
        return {};
    auto faces = readFaces(assets, false, current);
    if (!faces || (current && !current()))
        return {};
    warmTextShaping(faces->sources.value("InstrumentSerif-Regular.ttf"), QStringLiteral("Kobalt"));
    return current && !current() ? OpeningFonts() : faces;
}

bool openingFontsPrepared(const AssetBundle &assets)
{
    return bool(preparedFaces(assets));
}

void setPreparedOpeningFonts(const AssetBundle &assets, OpeningFonts faces)
{
    if (!faces || faces->generation == assets.generation())
        registration().prepared = std::move(faces);
}

bool initializeOpeningFonts(const AssetBundle &assets)
{
    auto faces = interfaceFaces(assets, true);
    if (!faces)
        return false;
    if (registration().active == faces)
        return true;
    bool changed = false;
    return installFaces(assets, faces, true, &changed);
}

bool initializeInterfaceFonts(const AssetBundle &assets, bool *changed)
{
    if (changed)
        *changed = false;
    const auto faces = interfaceFaces(assets, false);
    if (!faces)
        return false;
    QStringList files;
    for (const auto *file : scriptFallbackFontFiles)
        files.append(QLatin1String(file));
    // Validate the fallbacks along with the required interface files. All
    // optional script fallbacks may have been set aside.
    const auto readers = assets.deferredFontData(files + interfaceFiles());
    bool facesChanged = false;
    if (readers.isEmpty() || !installFaces(assets, faces, false, &facesChanged))
        return false;
    // Native editors need only the scripts actually present in their text.
    // Private text has its own lazy fallback selection and byte budget.
    auto &current = registration();
    for (const int id : current.scriptFallbackIds)
        QFontDatabase::removeApplicationFont(id);
    current.scriptFallbackIds.clear();
    current.fallbackSources.clear();
    ++current.fallbackGeneration;
    setDiagnosticNativeFontBytes(0);
    current.fallbackReaders.clear();
    for (const auto *file : scriptFallbackFontFiles)
        current.fallbackReaders.insert(file, readers.value(file));
    // Some sleep screens use bundled book faces: Jost for figures and Libron
    // for the letter. Registration records the files; bytes load when drawn.
    const auto bookFace = [&assets](const char *face, const QString &family) {
        const auto name = QStringLiteral("fonts/ebook-core/") + QLatin1String(face);
        if (!assets.setAside(name))
            registerPrivateInterfaceFont(assets.file(name), family);
    };
    for (const auto *face : { "NV_Jost-Regular.ttf", "NV_Jost-Bold.ttf" })
        bookFace(face, QStringLiteral("NV Jost"));
    for (const auto *face : { "Libron-Regular.ttf", "Libron-Italic.ttf" })
        bookFace(face, QStringLiteral("Libron"));
    if (changed) {
        *changed = facesChanged;
    }
    return true;
}

namespace {
class EditorFallbacks : public QObject
{
public:
    explicit EditorFallbacks(QLineEdit *editor) : QObject(editor)
    {
        editor->installEventFilter(this);
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Paint || preparing)
            return false;
        auto *editor = static_cast<QLineEdit *>(object);
        const auto text = editor->text().isEmpty() ? editor->placeholderText() : editor->text();
        const auto font = editor->font();
        auto &current = registration();
        if (text == previousText && font == previousFont
            && generation == current.fallbackGeneration)
            return false;
        preparing = true;
        const auto primary = QRawFont::fromFont(font);
        bool added = false;
        for (const auto character : text.toUcs4()) {
            if (primary.isValid() && primary.supportsCharacter(character))
                continue;
            QString stem;
            switch (QChar::script(character)) {
            case QChar::Script_Arabic:
                stem = "NotoNaskhArabic";
                break;
            case QChar::Script_Hebrew:
                stem = "NotoSerifHebrew";
                break;
            case QChar::Script_Armenian:
                stem = "NotoSerifArmenian";
                break;
            case QChar::Script_Georgian:
                stem = "NotoSerifGeorgian";
                break;
            case QChar::Script_Devanagari:
                stem = "NotoSerifDevanagari";
                break;
            case QChar::Script_Bengali:
                stem = "NotoSerifBengali";
                break;
            case QChar::Script_Tamil:
                stem = "NotoSerifTamil";
                break;
            case QChar::Script_Thai:
                stem = "NotoSerifThai";
                break;
            default:
                continue;
            }
            const bool bold = font.weight() >= QFont::Bold;
            const bool italic = font.italic() && stem == "NotoSerifTamil";
            const auto file = stem
                    + (bold ? (italic ? "-BoldItalic.ttf" : "-Bold.ttf")
                            : (italic ? "-Italic.ttf" : "-Regular.ttf"));
            if (current.fallbackSources.contains(file))
                continue;
            const auto read = current.fallbackReaders.value(file);
            const auto bytes = read ? read() : QByteArray();
            if (bytes.isEmpty())
                continue;
            const int id = QFontDatabase::addApplicationFontFromData(bytes);
            if (id < 0)
                continue;
            added = true;
            current.scriptFallbackIds.append(id);
            current.fallbackSources.insert(file, bytes);
            qint64 retained = 0;
            for (const auto &value : current.fallbackSources)
                retained += value.size();
            setDiagnosticNativeFontBytes(retained);
            diagnostic("font-native-fallback-load", { { "file_bytes", bytes.size() } }, false);
        }
        if (added)
            editor->setFont(renewedFont(font));
        generation = current.fallbackGeneration;
        previousText = text;
        previousFont = font;
        preparing = false;
        return false;
    }

private:
    bool preparing = false;
    unsigned generation = 0;
    QString previousText;
    QFont previousFont;
};
} // namespace

void prepareNativeEditor(QLineEdit *editor)
{
    new EditorFallbacks(editor);
}

bool setInterfaceWeight(int steps)
{
    const int previous = requestedSteps;
    requestedSteps = qMax(0, steps);
    return requestedSteps != previous;
}

int interfaceWeight()
{
    return requestedSteps;
}

QFont renewedFont(const QFont &font)
{
    // A copy shares the loaded faces, so build a new font from the attributes.
    // Qt 5.2 cannot read back its own description once a font has a hinting
    // preference, so the description is not used.
    QFont renewed(font.family());
    if (font.pixelSize() > 0)
        renewed.setPixelSize(font.pixelSize());
    else if (font.pointSizeF() > 0)
        renewed.setPointSizeF(font.pointSizeF());
    renewed.setWeight(font.weight());
    renewed.setStyle(font.style());
    renewed.setUnderline(font.underline());
    renewed.setStrikeOut(font.strikeOut());
    renewed.setFixedPitch(font.fixedPitch());
    renewed.setStyleHint(font.styleHint(), font.styleStrategy());
    renewed.setStretch(font.stretch());
    renewed.setCapitalization(font.capitalization());
    renewed.setLetterSpacing(font.letterSpacingType(), font.letterSpacing());
    renewed.setWordSpacing(font.wordSpacing());
    renewed.setKerning(font.kerning());
    renewed.setHintingPreference(font.hintingPreference());
    return renewed;
}

void refreshInterfaceText(QWidget *root, bool restyled)
{
    // Qt keeps the faces that a font object has loaded. After a font database
    // change it drops them only for fonts in its cache, and only once, so
    // widgets need new font objects. Setting the root's style sheet again
    // repolishes the root and every widget under it once. That parses new rule
    // fonts, resolves each widget's font anew and gives back the sheet colors
    // of widgets whose palette was set since their last polish. Setting each
    // widget's sheet again would repolish a widget once for every styled
    // ancestor, and took two thirds of a theme change.
    if (!restyled && !root->styleSheet().isEmpty())
        root->setStyleSheet(root->styleSheet());
    auto widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    QEvent change(QEvent::FontChange);
    for (auto *widget : widgets) {
        // Labels lay out rich text again, and paged text renews its document font.
        QCoreApplication::sendEvent(widget, &change);
        widget->update();
    }
}

namespace {
class UnhintedFonts final : public QObject
{
public:
    explicit UnhintedFonts(QWidget *window) : QObject(window), window(window) { }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::FontChange && event->type() != QEvent::Polish)
            return false;
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget || widget->font().hintingPreference() == QFont::PreferNoHinting
            || widget->window() != window)
            return false;
        // Setting the font again sends another font change, which now matches.
        auto font = widget->font();
        font.setHintingPreference(QFont::PreferNoHinting);
        widget->setFont(font);
        return false;
    }

private:
    QWidget *window;
};
} // namespace

int opticalCenterShift(const QFont &first, const QFont &last)
{
    const int capitals = interfaceInkBounds(QStringLiteral("H"), first).height();
    const int descent = interfaceLineHeight(last) - interfaceLineAscent(last);
    return qRound((interfaceLineAscent(first) - capitals - descent) / 2.0);
}

void keepInterfaceUnhinted(QWidget *window)
{
    QCoreApplication::instance()->installEventFilter(new UnhintedFonts(window));
}

void setInterfaceFontFamily(const QString &family)
{
    selectedInterfaceFamily = family;
}

void setInterfaceMonoFontFamily(const QString &family)
{
    selectedInterfaceMonoFamily = family;
}

void setHeaderFooterAddedWeight(int steps)
{
    stripAddedWeight = steps >= 0 && steps <= 8 && steps % 2 == 0 ? steps : 2;
}

int headerFooterAddedWeight()
{
    return stripAddedWeight;
}

int headerFooterTextWeight()
{
    return stripAddedWeight ? interfaceWeight() + stripAddedWeight : -1;
}

void setHeaderFooterFontFamily(const QString &family)
{
    selectedHeaderFooterFamily = family;
}

bool interfaceFontsMatch(const QString &mainFace, const QString &monoFace, const QString &stripFace,
                         int stripWeight)
{
    return mainFace == selectedInterfaceFamily && monoFace == selectedInterfaceMonoFamily
            && stripFace == selectedHeaderFooterFamily && stripWeight == stripAddedWeight;
}

QFont headerFooterFont(int pixels)
{
    return sizedFont(selectedHeaderFooterFamily.isEmpty() ? family(FamilyIndex::Mono)
                                                          : selectedHeaderFooterFamily,
                     pixels);
}

QFont interfaceFont(int pixels)
{
    return sizedFont(selectedInterfaceFamily.isEmpty() ? family(FamilyIndex::Sans)
                                                       : selectedInterfaceFamily,
                     pixels);
}

QFont chromeFont(int pixels)
{
    return sizedFont(selectedInterfaceMonoFamily.isEmpty() ? family(FamilyIndex::Mono)
                                                           : selectedInterfaceMonoFamily,
                     pixels);
}

QFont captionFont(int pixels)
{
    QFont font = chromeFont(pixels);
    font.setLetterSpacing(QFont::AbsoluteSpacing, metrics::chrome::captionSpacing);
    font.setCapitalization(QFont::AllUppercase);
    return font;
}

QFont brandFont(int pixels, bool italic)
{
    QFont font(family(FamilyIndex::Serif, "serif"));
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(pixels);
    font.setWeight(QFont::Normal);
    font.setItalic(italic);
    return font;
}

} // namespace kobalt::ui
