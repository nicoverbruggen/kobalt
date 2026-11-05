#include "platforms/kobo/common/native_font_scale.h"
#include "check.h"
#include <QApplication>
#include <QProxyStyle>
#include <QSet>
#include <QWidget>

namespace {
class Polishes final : public QProxyStyle
{
public:
    void polish(QWidget *widget) override
    {
        polished.insert(widget);
        QProxyStyle::polish(widget);
    }

    using QProxyStyle::polish;
    QSet<QWidget *> polished;
};

bool scaled()
{
    return qApp->property("fontScale").isValid();
}
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    using kobalt::nickel::ignoreNativeFontScale;
    using kobalt::nickel::NativeFontScaleScope;

    using kobalt::nickel::nativeFontScale;

    {
        NativeFontScaleScope building;
    }
    require(!scaled(), "a scope without Large Print must not add the property");
    require(nativeFontScale() == 1, "the interface keeps its size without Large Print");

    app.setProperty("fontScale", 2.0f);
    require(nativeFontScale() == 2, "the interface takes Nickel's largest step");
    app.setProperty("fontScale", 1.25f);
    require(nativeFontScale() == 1.25, "the interface takes Nickel's scale");
    {
        NativeFontScaleScope building;
        require(!scaled(), "Kobalt windows must be built without Nickel's font scale");
        require(nativeFontScale() == 1.25, "the scale must stay readable while it is held");
        {
            NativeFontScaleScope nested;
        }
        require(!scaled(), "a nested scope must not restore the scale early");
    }
    require(app.property("fontScale").toFloat() == 1.25f, "the scope must restore Nickel's value");

    auto *opening = new QWidget;
    auto *reader = new QWidget;
    {
        NativeFontScaleScope building;
        ignoreNativeFontScale(opening);
        ignoreNativeFontScale(reader);
    }
    require(!scaled(), "a window not yet shown must keep the scale out");
    opening->show();
    reader->show();
    opening->hide();
    require(!scaled(), "the scale must stay out while any Kobalt window is visible");
    reader->hide();
    require(scaled(), "Nickel's windows must regain the scale when Kobalt hides");
    reader->show();
    require(!scaled(), "showing a Kobalt window again must clear the scale");
    delete reader;
    require(scaled(), "destroying the last visible Kobalt window must restore the scale");
    opening->show();
    delete opening;
    require(app.property("fontScale").toFloat() == 1.25f,
            "the restored value must be Nickel's value");

    // Nickel widgets restyled without the scale must be styled again with it.
    Polishes style;
    QWidget library;
    library.setStyle(&style);
    auto *reading = new QWidget;
    auto *panel = new QWidget(reading);
    panel->setStyle(&style);
    {
        NativeFontScaleScope building;
        ignoreNativeFontScale(reading);
        reading->show();
        style.polished.clear();
        QFont changed = library.font();
        changed.setPixelSize(37);
        library.setFont(changed);
        panel->setFont(changed);
    }
    reading->hide();
    QCoreApplication::processEvents();
    require(style.polished.contains(&library),
            "a Nickel widget restyled during the hold must be polished again");
    require(!style.polished.contains(panel), "a Kobalt widget must not be polished again");
    style.polished.clear();
    delete reading;
    QCoreApplication::processEvents();
    require(style.polished.isEmpty(),
            "a widget restyled outside a hold must not be polished again");

    // A Nickel dialog over Kobalt keeps Nickel's scale until it hides.
    using kobalt::nickel::allowNativeFontScale;
    QWidget unheld;
    allowNativeFontScale(&unheld, true);
    require(scaled(), "a dialog without a Kobalt window changes nothing");
    auto *host = new QWidget;
    auto *hostPanel = new QWidget(host);
    hostPanel->setStyle(&style);
    {
        NativeFontScaleScope building;
        ignoreNativeFontScale(host);
        host->show();
    }
    require(!scaled(), "the host keeps the scale out");
    QWidget dialog;
    allowNativeFontScale(&dialog, true);
    require(scaled(), "a Nickel dialog is polished with Nickel's scale");
    dialog.show();
    style.polished.clear();
    QFont changed = hostPanel->font();
    changed.setPixelSize(41);
    hostPanel->setFont(changed);
    dialog.hide();
    require(!scaled(), "the host keeps the scale out again once the dialog hides");
    QCoreApplication::processEvents();
    require(style.polished.contains(hostPanel),
            "a Kobalt widget restyled under the dialog's scale is polished again");
    QWidget early;
    auto *child = new QWidget(&early);
    child->setStyle(&style);
    early.setStyle(&style);
    early.show();
    style.polished.clear();
    allowNativeFontScale(&early, false);
    require(scaled() && style.polished.contains(&early) && style.polished.contains(child),
            "a dialog polished before it showed over Kobalt is polished again");
    early.hide();
    require(!scaled(), "a hidden dialog no longer holds the scale");
    allowNativeFontScale(host, true);
    require(!scaled(), "Kobalt's own windows never take Nickel's scale");
    delete host;
    QCoreApplication::processEvents();
    require(scaled(), "Nickel's scale returns when the host closes");

    auto *closing = new QWidget;
    ignoreNativeFontScale(closing);
    app.setProperty("fontScale", 1.5f);
    delete closing;
    require(app.property("fontScale").toFloat() == 1.5f,
            "a newer Nickel value must not be replaced");
}
