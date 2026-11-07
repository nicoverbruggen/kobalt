#include "about_page.h"
#include "ui/controls/buttons.h"
#include "ui/controls/panels.h"
#include "ui/controls/rows.h"
#include "ui/controls/text.h"
#include "ui/style/fonts.h"
#include "ui/style/metrics.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include "build_info.h"
#include <QGridLayout>
#include <QVBoxLayout>

namespace kobalt {
namespace metrics = ui::metrics;
namespace chrome = ui::metrics::chrome;
namespace styles = ui::styles;
using ui::readerStyle;

AboutPage::AboutPage(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrAbout");
    auto *aboutLayout = new QGridLayout(this);
    aboutLayout->setContentsMargins(metrics::about::margins);
    auto *aboutContent = new QWidget(this);
    aboutContent->setObjectName("rrAboutContent");
    aboutLayout->addWidget(aboutContent, 0, 0, Qt::AlignCenter);
    auto *aboutRows = new QVBoxLayout(aboutContent);
    aboutRows->setContentsMargins(0, 0, 0, 0);
    aboutRows->setSpacing(metrics::about::gap);
    auto *logo = new kobalt::ui::BrandIcon(this);
    logo->setObjectName("rrAboutLogo");
    logo->setFixedSize(metrics::about::logoSize, metrics::about::logoSize);
    aboutRows->addWidget(logo, 0, Qt::AlignHCenter);
    auto *wordmark = new ui::InterfaceLabel(strings::get("app.wordmark"), this);
    wordmark->setObjectName("rrAboutWordmark");
    wordmark->setAlignment(Qt::AlignCenter);
    readerStyle(wordmark, styles::brand(metrics::about::wordmarkText));
    aboutRows->addWidget(wordmark);
    // A short rule above and below sets the build apart from the name and the description.
    const auto buildRule = [&](const char *name) {
        auto *rule = new QFrame(this);
        rule->setObjectName(name);
        rule->setFixedSize(metrics::about::ruleWidth, metrics::control::border);
        readerStyle(rule, "border:none;background:palette(light);");
        aboutRows->addWidget(rule, 0, Qt::AlignHCenter);
        return rule;
    };
    // The rules keep more room on their outer side than next to the build lines.
    aboutRows->addSpacing(metrics::about::sectionGap);
    buildStart = buildRule("rrAboutBuildTop");
    auto *build = new QVBoxLayout;
    build->setSpacing(metrics::about::buildGap);
    aboutRows->addLayout(build);
    // "Version 0.4 (Qt6)", then the short commit with "*" for uncommitted changes.
    const auto release = QStringLiteral(KOBALT_VERSION);
    const auto displayRelease = release.mid(release.startsWith('v') ? 1 : 0);
    auto *version = new ui::InterfaceLabel(
            strings::get("about.version").arg(displayRelease).arg(QT_VERSION >> 16), this);
    version->setObjectName("rrAboutVersion");
    version->setAlignment(Qt::AlignCenter);
    build->addWidget(version);
    auto *commit = new ui::InterfaceLabel(
            QString::fromLatin1(kobaltCommit) + (KOBALT_COMMIT_MODIFIED ? "*" : ""), this);
    commit->setObjectName("rrAboutCommit");
    commit->setAlignment(Qt::AlignCenter);
    build->addWidget(commit);
    commit->setVisible(kobaltCommit[0] != '\0');
    buildRule("rrAboutBuildBottom");
    aboutRows->addSpacing(metrics::about::sectionGap);
    description = new ui::InterfaceLabel(strings::get("about.description"), this);
    description->setObjectName("rrAboutDescription");
    description->setWordWrap(true);
    description->setAlignment(Qt::AlignCenter);
    description->setFixedWidth(metrics::about::textWidth);
    readerStyle(description, styles::font(metrics::text::body));
    description->ensurePolished();
    // Measure at the displayed width so Qt cannot clip wrapped lines.
    description->setFixedHeight(description->heightForWidth(metrics::about::textWidth));
    aboutRows->addWidget(description, 0, Qt::AlignHCenter);
    // The paragraph sits evenly between the rule and the button.
    aboutRows->addSpacing(metrics::about::buttonGap);
    auto *aboutActions = kobalt::ui::horizontalRow();
    aboutRows->addLayout(aboutActions);
    aboutActions->addStretch(1);
    acknowledgmentsButton = new ui::Button(strings::get("acknowledgments.acknowledgments"), this);
    acknowledgmentsButton->setObjectName("rrOpenAcknowledgments");
    acknowledgmentsButton->setFocusPolicy(Qt::NoFocus);
    aboutActions->addWidget(acknowledgmentsButton);
    aboutActions->addStretch(1);
    ui::readerStyle(description, styles::font(chrome::labelText));
    ui::readerStyle(version, styles::font(chrome::labelText));
    ui::readerStyle(commit, styles::font(metrics::about::commitText));
    ui::readerStyle(acknowledgmentsButton,
                    styles::outlined() + "QPushButton{" + styles::font(chrome::labelText) + "}");
}

int AboutPage::fitWidth(int width)
{
    description->setFixedWidth(qMax(1, qMin<int>(metrics::about::textWidth, width - ui::px(32))));
    description->setMinimumHeight(0);
    description->setMaximumHeight(QWIDGETSIZE_MAX);
    description->setFixedHeight(description->heightForWidth(description->width()));
    description->parentWidget()->layout()->activate();
    return layout()->minimumSize().height();
}

int AboutPage::descriptionTop() const
{
    return description->mapTo(this, QPoint()).y();
}

QVector<int> AboutPage::pageStarts(int availableHeight) const
{
    const int buildTop = buildStart->mapTo(this, QPoint()).y() - int(metrics::about::sectionGap);
    const int descriptionStart = descriptionTop() - int(metrics::about::sectionGap);
    QVector<int> starts{ 0 };
    int start = 0, previous = 0;
    // Keep the build lines together. The description and its action also stay
    // together when they fit, just as they do in reader Settings.
    for (int end : { buildTop, descriptionStart, height() }) {
        if (end - start > availableHeight && previous > start) {
            start = previous;
            starts.append(start);
        }
        while (end - start > availableHeight) {
            start += availableHeight;
            starts.append(start);
        }
        previous = end;
    }
    return starts;
}

} // namespace kobalt
