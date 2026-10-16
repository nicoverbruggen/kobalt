#include "fonts.h"
#include "font_data.h"
#include "private_text.h"
#include "assets/assets.h"
#include "assets/bundled_fonts.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QLabel>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <QMap>

namespace kobalt::ui {
namespace {
struct Face {
    const char *file, *style;
};
struct Family {
    const char *name;
    QVector<Face> faces;
};
// The interface registers its own renamed copies of the bundled faces. Their
// names never match a reading font that the font picker registers, so
// replacing them to change the weight cannot pick up the picker's faces.
// IBM Plex reserves the name "Plex" for the original fonts.
enum FamilyIndex { Sans, Mono, Serif };
const QVector<Family> &families() {
    static const QVector<Family> value{
        {"Kobalt UI Sans",
         {{"IBMPlexSans-Regular.ttf", "Regular"},
          {"IBMPlexSans-Bold.ttf", "Bold"},
          {"IBMPlexSans-Italic.ttf", "Italic"}}},
        {"Kobalt UI Mono", {{"IBMPlexMono-Regular.ttf", "Regular"}, {"IBMPlexMono-Bold.ttf", "Bold"}}},
        {"Kobalt UI Serif", {{"InstrumentSerif-Regular.ttf", "Regular"}, {"InstrumentSerif-Italic.ttf", "Italic"}}},
    };
    return value;
}
struct Registration {
    int steps = -1;
    QVector<QVector<int>> ids;
    QVector<int> scriptFallbackIds;
    // Interface faces and script fallbacks load separately: the opening view
    // needs only the faces, before the whole asset bundle is verified.
    QMap<QString, QByteArray> sources, fallbackSources;
    FontCopies copies;
};
Registration &registration() {
    static Registration value;
    return value;
}
int requestedSteps = 0;
int faceCount(const QVector<QVector<int>> &ids) {
    int count = 0;
    for (const auto &family : ids)
        count += family.size();
    return count;
}

// Each weight has its own file name. Qt keeps FreeType faces by file name, so
// a face that a live layout still holds can never stand in for another weight.
QVector<QVector<int>> addFaces(int steps) {
    QVector<QVector<int>> ids;
    for (const auto &family : families()) {
        ids.append(QVector<int>());
        // Qt 5.2 files a face under the first face registered for its weight
        // and slant, so the regular face comes first.
        for (const auto &face : family.faces) {
            const auto data = interfaceFace(registration().sources.value(face.file), family.name, face.style, steps);
            const auto path = data.isEmpty()
                                  ? QString()
                                  : registration().copies.file(QString("weight-%1/%2").arg(steps).arg(face.file), data);
            const int id = path.isEmpty() ? -1 : QFontDatabase::addApplicationFont(path);
            if (id >= 0)
                ids.last().append(id);
        }
    }
    return ids;
}
QVector<int> addScriptFallbacks(const QMap<QString, QByteArray> &sources) {
    QVector<int> ids;
    for (const auto *file : scriptFallbackFontFiles) {
        const auto bytes = sources.value(file);
        const int id = bytes.isEmpty() ? -1 : QFontDatabase::addApplicationFontFromData(bytes);
        if (id < 0) {
            for (const int added : ids)
                QFontDatabase::removeApplicationFont(added);
            return {};
        }
        ids.append(id);
    }
    return ids;
}
void registerFaces() {
    auto &current = registration();
    // Before the faces are read, a use falls back without recording a weight,
    // so the first loaded faces still register.
    if (current.steps == requestedSteps || current.sources.isEmpty())
        return;
    auto added = addFaces(requestedSteps);
    int expected = 0;
    for (const auto &family : families())
        expected += family.faces.size();
    // Keep the faces in use when the new weight cannot be read in full.
    if (current.steps >= 0 && faceCount(added) < expected) {
        for (const auto &ids : added)
            for (const int id : ids)
                QFontDatabase::removeApplicationFont(id);
        requestedSteps = current.steps;
        return;
    }
    for (const auto &ids : current.ids)
        for (const int id : ids)
            QFontDatabase::removeApplicationFont(id);
    current.steps = requestedSteps;
    current.ids = std::move(added);
}
QString family(FamilyIndex index, const QString &missing = {}) {
    registerFaces();
    return registration().ids.value(index).isEmpty() ? missing : QString::fromLatin1(families()[index].name);
}
QFont sizedFont(const QString &family, int pixels) {
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
}
namespace {
// Replaces the registered faces when their bytes change. Keeps the faces in
// use when the new set cannot be registered in full.
bool replaceFaces(QMap<QString, QByteArray> sources, bool *changed) {
    auto &current = registration();
    if (current.sources == sources)
        return true;
    if (current.steps < 0) {
        // Nothing has drawn with the faces yet. They register on first use.
        current.sources = std::move(sources);
        *changed = true;
        return true;
    }
    auto old = std::move(current.sources);
    current.sources = std::move(sources);
    auto added = addFaces(requestedSteps);
    if (faceCount(added) != current.sources.size()) {
        for (const auto &family : added)
            for (const int id : family)
                QFontDatabase::removeApplicationFont(id);
        current.sources = std::move(old);
        return false;
    }
    for (const auto &family : current.ids)
        for (const int id : family)
            QFontDatabase::removeApplicationFont(id);
    current.steps = requestedSteps;
    current.ids = std::move(added);
    *changed = true;
    return true;
}
}
bool initializeOpeningFonts(const AssetBundle &assets) {
    // A previous opening registered verified faces. The full check after the
    // first paint replaces them if the bundle changed.
    if (!registration().sources.isEmpty())
        return true;
    QMap<QString, QByteArray> sources;
    for (const auto &family : families())
        for (const auto &face : family.faces) {
            const auto bytes = assets.manifestFontData(face.file);
            if (bytes.isEmpty())
                return false;
            sources.insert(face.file, bytes);
        }
    bool changed = false;
    return replaceFaces(std::move(sources), &changed);
}
bool initializeInterfaceFonts(const AssetBundle &assets, bool *changed) {
    bool facesChanged = false;
    if (changed) {
        *changed = false;
    }
    if (!initializePrivateInterfaceText(assets))
        return false;
    QMap<QString, QByteArray> sources, fallbackSources;
    for (const auto &family : families())
        for (const auto &face : family.faces) {
            const auto bytes = assets.fontData(face.file);
            if (bytes.isEmpty())
                return false;
            sources.insert(face.file, bytes);
        }
    for (const auto *file : scriptFallbackFontFiles) {
        const auto bytes = assets.fontData(file);
        if (bytes.isEmpty())
            return false;
        fallbackSources.insert(file, bytes);
    }
    auto &current = registration();
    if (current.fallbackSources != fallbackSources) {
        auto scriptFallbackIds = addScriptFallbacks(fallbackSources);
        if (scriptFallbackIds.size() != int(scriptFallbackFontFiles.size()))
            return false;
        for (const int id : current.scriptFallbackIds)
            QFontDatabase::removeApplicationFont(id);
        current.fallbackSources = std::move(fallbackSources);
        current.scriptFallbackIds = std::move(scriptFallbackIds);
    }
    if (!replaceFaces(std::move(sources), &facesChanged))
        return false;
    if (changed) {
        *changed = facesChanged;
    }
    return true;
}
bool setInterfaceWeight(int steps) {
    steps = qMax(0, steps);
    const int previous = registration().steps;
    requestedSteps = steps;
    // Before the first use, only remember the weight.
    if (previous < 0)
        return false;
    registerFaces();
    return registration().steps != previous;
}
int interfaceWeight() {
    return registration().steps < 0 ? requestedSteps : registration().steps;
}
QFont renewedFont(const QFont &font) {
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
void refreshInterfaceText(QWidget *root) {
    // Qt keeps the faces that a font object has loaded. After a font database
    // change it drops them only for fonts in its cache, and only once, so
    // widgets need new font objects. Setting a style sheet again parses new
    // rule fonts and resolves the fonts of the widget and its children anew.
    auto widgets = root->findChildren<QWidget *>();
    widgets.prepend(root);
    QEvent change(QEvent::FontChange);
    for (auto *widget : widgets) {
        if (const auto sheet = widget->styleSheet(); !sheet.isEmpty()) {
            widget->setStyleSheet(QString());
            widget->setStyleSheet(sheet);
        }
        // Labels lay out rich text again, and paged text renews its document font.
        QCoreApplication::sendEvent(widget, &change);
        widget->update();
    }
}
namespace {
class UnhintedFonts final : public QObject {
public:
    explicit UnhintedFonts(QWidget *window) : QObject(window), window(window) {}
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() != QEvent::FontChange && event->type() != QEvent::Polish)
            return false;
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget || widget->font().hintingPreference() == QFont::PreferNoHinting || widget->window() != window)
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
}
int opticalCenterShift(const QFont &first, const QFont &last) {
    const QFontMetricsF top(first), bottom(last);
    const qreal capitals = top.tightBoundingRect(QStringLiteral("H")).height();
    return qRound((top.ascent() - capitals - bottom.descent()) / 2);
}
void keepInterfaceUnhinted(QWidget *window) {
    QCoreApplication::instance()->installEventFilter(new UnhintedFonts(window));
}
QFont interfaceFont(int pixels) {
    return sizedFont(family(Sans), pixels);
}
QFont chromeFont(int pixels) {
    return sizedFont(family(Mono), pixels);
}
QFont captionFont(int pixels) {
    QFont font = chromeFont(pixels);
    font.setLetterSpacing(QFont::AbsoluteSpacing, metrics::chrome::captionSpacing);
    font.setCapitalization(QFont::AllUppercase);
    return font;
}
QFont brandFont(int pixels, bool italic) {
    QFont font(family(Serif, "serif"));
    font.setHintingPreference(QFont::PreferNoHinting);
    font.setPixelSize(pixels);
    font.setWeight(QFont::Normal);
    font.setItalic(italic);
    return font;
}

}
