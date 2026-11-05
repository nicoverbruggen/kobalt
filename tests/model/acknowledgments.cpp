#include "reader_inputs.h"
#include "ui/style/fonts.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "app/reader/settings/acknowledgments.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>
#include <QFontInfo>
#include <QStringList>
#include <cstdio>
#include <cstdlib>
#include "check.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    require(kobalt::test::readerAssets().available(),
            "Component assets failed manifest verification");
    require(kobalt::ui::initializeInterfaceFonts(kobalt::test::readerAssets()),
            "Cannot load verified UI font bytes");
    require(QFontInfo(kobalt::ui::interfaceFont()).family() == "IBM Plex Sans"
                    && QFontInfo(kobalt::ui::chromeFont()).family() == "IBM Plex Mono"
                    && QFontInfo(kobalt::ui::brandFont(32)).family() == "Instrument Serif",
            "Component font substitution");
    QFile manifest(":/kobalt/acknowledgments.json");
    require(manifest.open(QIODevice::ReadOnly), "Embedded credits are missing");
    QJsonParseError error;
    const auto entries = QJsonDocument::fromJson(manifest.readAll(), &error).array();
    require(error.error == QJsonParseError::NoError && !entries.isEmpty(),
            "Invalid credits manifest");
    // Groups are contiguous blocks, and the author's own projects form their own group.
    QStringList groups, author;
    for (const auto &entry : entries) {
        const auto group = entry.toObject().value("group").toString();
        require(QStringList({ "kobalt", "engine", "nickel", "fonts", "icons", "author" })
                        .contains(group),
                "Project has no known group");
        if (groups.isEmpty() || groups.last() != group) {
            require(!groups.contains(group), "A group is split across the list");
            groups.append(group);
        }
        if (group == "author")
            author.append(entry.toObject().value("name").toString());
    }
    require(author == QStringList({ "NickelRotate", "NickelDissolve", "eBook Fonts" }),
            "The author's projects are not grouped");
    // Kobalt's own license terms come first, with both license texts.
    const auto kobalt = entries.first().toObject();
    const auto licenses = kobalt.value("notices").toArray();
    require(kobalt.value("group").toString() == "kobalt"
                    && kobalt.value("name").toString() == "Kobalt" && licenses.size() == 2
                    && licenses.at(0).toString() == "Kobalt-MIT"
                    && licenses.at(1).toString() == "AGPL-3",
            "Kobalt's own license is not listed first");
    for (const auto &entry : entries) {
        const auto notices = entry.toObject().value("notices").toArray();
        require(!notices.isEmpty(), "Project has no license notice");
        for (const auto &notice : notices) {
            QFile file(":/kobalt/notices/" + notice.toString());
            require(file.open(QIODevice::ReadOnly) && !file.readAll().trimmed().isEmpty(),
                    "Embedded notice is missing or empty");
        }
    }
    kobalt::Acknowledgments panel(nullptr);
    panel.resize(1072, 1448);
    panel.open();
    app.processEvents();
    auto button = [&](const char *name) {
        auto *found = panel.findChild<QPushButton *>(name);
        require(found != nullptr, "Navigation button missing");
        return found;
    };
    auto click = [&](const char *name) {
        button(name)->click();
        app.processEvents();
    };
    auto *counter = panel.findChild<QLabel *>("rrAcknowledgmentsPage");
    auto *heading = panel.findChild<QLabel *>("rrAcknowledgmentsGroup_kobalt");
    require(heading && heading->isVisibleTo(&panel)
                    && heading->text() == kobalt::strings::get("acknowledgments.group_kobalt"),
            "The first page does not start with the Kobalt heading");
    auto *engine = panel.findChild<QLabel *>("rrAcknowledgmentsGroup_engine");
    require(engine, "The engine heading is missing");
    bool statement = false;
    for (auto *label : panel.findChildren<QLabel *>())
        statement |= label->isVisibleTo(&panel)
                && label->text().startsWith(kobalt::strings::get(
                        "acknowledgments.kobalt_is_distributed_under_the_gnu_agpl_v3_without_any_warranty"));
    require(statement, "The license and warranty statement is not shown");
    require(button("rrAcknowledgmentsPrevious")->isEnabled(),
            "Previous cannot wrap on first list page");
    click("rrAcknowledgmentsPrevious");
    const auto finalListPage = counter->text().split(" of ");
    require(finalListPage.first() == finalListPage.last(),
            "Acknowledgments did not wrap backwards");
    click("rrAcknowledgmentsNext");
    require(counter->text().startsWith("1 of"), "Acknowledgments did not wrap forwards");
    click("rrAcknowledgmentsNext");
    require(counter->text().startsWith("2 of"), "Project list did not advance");
    click("rrAcknowledgmentsPrevious");
    click("rrAcknowledgment0");
    require(button("rrAcknowledgmentsNext")->isEnabled(), "Long license was not paginated");
    auto *license = panel.findChild<QWidget *>("rrLicenseText");
    require(QFontInfo(license->font()).fixedPitch(), "License font is not monospace");
    auto checkBottom = [&] {
        auto *text = panel.findChild<QWidget *>("rrLicenseText");
        const auto image = text->grab().toImage();
        const auto paper = text->palette().color(QPalette::Window);
        for (int y = image.height() - 4; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                require(QColor::fromRgba(image.pixel(x, y)) == paper,
                        "License line reaches the screen cut-off");
    };
    checkBottom();
    click("rrAcknowledgmentsNext");
    require(counter->text().startsWith("2 of"), "License did not advance");
    panel.resize(1448, 1072);
    app.processEvents();
    require(counter->text().startsWith("2 of"), "Rotation lost the license page");
    int turns = 0;
    while (button("rrAcknowledgmentsNext")->isEnabled() && turns++ < 200) {
        click("rrAcknowledgmentsNext");
        checkBottom();
    }
    require(turns < 200, "License paging did not stop at its end");
    click("rrAcknowledgmentsBack");
    auto *title = panel.findChild<QLabel *>("rrAcknowledgmentsTitle");
    require(title->text() == kobalt::strings::get("acknowledgments.acknowledgments"),
            "Back did not return to projects");
    // On a Libra Colour, a short notice fits on one page and its pager stays hidden.
    panel.resize(1264, 1680);
    app.processEvents();
    int index = -1;
    for (int i = 0; i < entries.size(); ++i)
        if (entries.at(i).toObject().value("name").toString() == "xxHash")
            index = i;
    require(index >= 0, "The short xxHash notice is missing");
    const auto name = QString("rrAcknowledgment%1").arg(index).toUtf8();
    for (int turns = 0; !(panel.findChild<QPushButton *>(name.constData())
                          && panel.findChild<QPushButton *>(name.constData())->isVisible())
         && turns < 10;
         ++turns)
        click("rrAcknowledgmentsNext");
    click(name.constData());
    require(title->text() == "xxHash" && !button("rrAcknowledgmentsNext")->isVisibleTo(&panel),
            "A one-page notice shows a pager");
    click("rrAcknowledgmentsBack");
    bool finished = false;
    panel.finished = [&] { finished = true; };
    click("rrAcknowledgmentsBack");
    require(finished, "Back did not return to settings");
    std::puts("PASS: embedded notices, project and license paging, rotation and back navigation");
}
