#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/rows.h"
#include "ui/controls/paging.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "dictionaries_page.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include "support/strings.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QResizeEvent>
#include <QPixmap>
#include <QTimer>
#include <QPainter>

namespace kobalt::reader {
namespace chrome = ui::metrics::chrome;
namespace metrics = ui::metrics;

namespace {
QString bytes(qint64 value)
{
    const bool gb = value >= 1000000000;
    const double amount = value / (gb ? 1000000000.0 : 1000000.0);
    return QString::number(amount, 'f', amount < 10 ? 1 : 0) + (gb ? " GB" : " MB");
}

class DictionaryTextRow final : public ui::ListRow
{
public:
    using ui::ListRow::ListRow;
    bool compact = false;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        if (!compact)
            ui::ListRow::paintEvent(event);
    }
};

class DictionaryLine : public QWidget
{
public:
    explicit DictionaryLine(QWidget *parent) : QWidget(parent)
    {
        name = new ui::InterfaceLabel(this);
        name->setWordWrap(true);
        ui::readerStyle(name, ui::styles::font(chrome::labelText) + "background:transparent;");
        metadata = new ui::InterfaceLabel(this);
        ui::captionLabel(metadata);
    }

    ui::InterfaceLabel *name, *metadata;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::paintEdges(painter, rect(), Qt::BottomEdge, ui::dividerColor(this));
    }
};

class StorageLabel : public ui::InterfaceLabel
{
public:
    using ui::InterfaceLabel::InterfaceLabel;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        ui::InterfaceLabel::paintEvent(event);
        QPainter painter(this);
        painter.setPen(ui::dividerColor(this));
        painter.drawLine(0, 0, width() - 1, 0);
    }
};

QIcon transferProgressIcon(int percent, const QColor &ink)
{
    QPixmap image(ui::px(28), ui::px(28));
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF circle = QRectF(image.rect()).adjusted(1, 1, -1, -1);
    painter.setPen(QPen(ink, metrics::control::border));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(circle);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPie(circle.adjusted(3, 3, -3, -3), 90 * 16, -percent * 360 * 16 / 100);
    painter.end();
    return QIcon(image);
}

struct Group
{
    QString section, language;
    QVector<DownloadDictionary> entries;
};

struct Item
{
    QString caption;
    bool language = false;
    DownloadDictionary entry;
    int height = 0;
};
} // namespace

DictionariesPage::DictionariesPage(QWidget *parent) : QWidget(parent)
{
    setObjectName("rrDictionaries");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    list = new QWidget(this);
    layout->addWidget(list, 1);
    pager = new ui::PagerBar(this, "rrDictionaries");
    pager->setObjectName("rrDictionariesPager");
    layout->addWidget(pager);
    storage = new StorageLabel(this);
    storage->setObjectName("rrDictionariesStorage");
    ui::captionLabel(storage);
    storage->setContentsMargins(chrome::inset, 0, chrome::inset, 0);
    storage->setFixedHeight(metrics::list::headerHeight);
    layout->addWidget(storage);
    connect(pager->previous, &QPushButton::clicked, this, [this] {
        failedId.clear();
        page = pager->turnTarget(-1);
        redraw();
    });
    connect(pager->next, &QPushButton::clicked, this, [this] {
        failedId.clear();
        page = pager->turnTarget(1);
        redraw();
    });
}

DictionariesPage::~DictionariesPage()
{
    delete confirmation;
}

int DictionariesPage::preferredHeight() const
{
    return parentWidget() ? parentWidget()->height() : height();
}

void DictionariesPage::hideEvent(QHideEvent *event)
{
    dismissConfirmation();
    QWidget::hideEvent(event);
}

void DictionariesPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    QTimer::singleShot(0, this, [this] { redraw(); });
}

void DictionariesPage::setLanguage(const QString &value)
{
    const auto next = DictionaryManager::normalizeLanguage(value);
    if (language == next)
        return;
    language = next;
    redraw();
}

void DictionariesPage::setCatalog(const QVector<DownloadDictionary> &value)
{
    entries = value;
    for (auto &entry : entries) {
        entry.language = DictionaryManager::normalizeLanguage(entry.language);
        entry.target = DictionaryManager::normalizeLanguage(entry.target);
    }
    entries = DictionaryManager::orderedCatalog(entries);
    redraw();
}

void DictionariesPage::setStorage(const QHash<QString, qint64> &sizes, qint64 available,
                                  const QString &)
{
    if (installedSizes == sizes && freeBytes == available)
        return;
    installedSizes = sizes;
    freeBytes = available;
    refreshStorage();
}

void DictionariesPage::setTransfer(const QString &id, qint64 received, qint64 total,
                                   bool installing)
{
    const bool same = transferId == id && receivedBytes == qMax<qint64>(0, received)
            && totalBytes == qMax<qint64>(0, total) && unpacking == installing;
    if (same)
        return;
    if (!transferId.isEmpty() && id.isEmpty())
        endedId = transferId;
    transferId = id;
    receivedBytes = qMax<qint64>(0, received);
    totalBytes = qMax<qint64>(0, total);
    unpacking = installing;
    redraw();
}

void DictionariesPage::setState(const QSet<QString> &installed, bool busy, const QString &message)
{
    const bool changed = installedIds != installed || working != busy;
    const auto previousFailure = failedId;
    installedIds = installed;
    working = busy;
    if (!endedId.isEmpty()) {
        if (!installed.contains(endedId) && !message.isEmpty()
            && !message.contains("cancel", Qt::CaseInsensitive))
            failedId = endedId;
        endedId.clear();
    }
    if (!busy && !requestedId.isEmpty()) {
        if (!installed.contains(requestedId) && !message.isEmpty()
            && !message.contains("cancel", Qt::CaseInsensitive))
            failedId = requestedId;
        requestedId.clear();
    }
    if (changed || previousFailure != failedId)
        redraw();
}

void DictionariesPage::dismissConfirmation()
{
    if (confirmation) {
        confirmation->hide();
        confirmation->setObjectName({});
        for (auto *child : confirmation->findChildren<QWidget *>())
            child->setObjectName({});
        confirmation->deleteLater();
        confirmation.clear();
    }
}

void DictionariesPage::askConfirmation(const DownloadDictionary &entry, bool removing)
{
    failedId.clear();
    if (confirmation)
        return;
    QWidget *owner = this;
    for (auto *parent = parentWidget(); parent; parent = parent->parentWidget())
        if (parent->objectName() == "rrGeneralSettings") {
            owner = parent->parentWidget();
            break;
        }
    auto license = entry.license;
    if (license.startsWith("CC-BY-SA-"))
        license = "CC BY-SA " + license.mid(9);
    else if (license.startsWith("CC-BY-"))
        license = "CC BY " + license.mid(6);
    const auto prefix = QString(removing ? "rrDictionaryRemove" : "rrDictionaryDownload");
    const auto description = removing
            ? strings::get("dictionaries.remove_description")
                      .arg(bytes(installedSizes.value(entry.id)))
            : strings::get("dictionaries.download_description").arg(bytes(entry.size), license);
    confirmation = new ui::ChoiceModal(
            owner,
            { description,
              strings::get(removing ? "dictionaries.remove_title" : "dictionaries.download_title")
                      .arg(entry.name),
              {} },
            { strings::get("common.cancel"), prefix + "Cancel" },
            { strings::get(removing ? "dictionaries.remove" : "dictionaries.download"),
              prefix + "Confirm" });
    confirmation->setObjectName(prefix);
    auto *dialog = confirmation.data();
    dialog->dismissed = [this] { dismissConfirmation(); };
    connect(dialog->secondaryButton(), &QPushButton::clicked, this,
            [this] { dismissConfirmation(); });
    connect(dialog->primaryButton(), &QPushButton::clicked, this,
            [this, dialog, id = entry.id, removing] {
                if (confirmation != dialog)
                    return;
                dismissConfirmation();
                if (removing) {
                    if (removeRequested)
                        removeRequested(id);
                } else if (!working && !installedIds.contains(id) && installRequested) {
                    requestedId = id;
                    installRequested(id);
                }
            });
    dialog->show();
    dialog->raise();
    dialog->setFocus();
}

void DictionariesPage::redraw()
{
    const auto previous = list->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    QSet<QWidget *> retained;
    bool matching = false;
    for (const auto &entry : entries)
        matching |= !language.isEmpty() && entry.language == language;
    QVector<Group> groups;
    for (int section = 0; section < 4; ++section) {
        if (!matching && (section == 0 || section == 2))
            continue;
        const bool installed = section < 2, forBook = section % 2 == 0;
        QString heading = (installed ? strings::get("dictionaries.section_installed")
                                     : strings::get("dictionaries.section_available"));
        if (matching)
            heading = forBook ? (installed ? strings::get("dictionaries.installed_book")
                                           : strings::get("dictionaries.available_book"))
                                        .arg(DictionaryManager::languageName(language))
                              : (installed ? strings::get("dictionaries.installed_other")
                                           : strings::get("dictionaries.available_other"));
        for (const auto &entry : entries) {
            if (installedIds.contains(entry.id) != installed
                || (matching && (entry.language == language) != forBook))
                continue;
            const QString sub =
                    section == 3 ? DictionaryManager::languageName(entry.language) : QString();
            if (groups.isEmpty() || groups.last().section != heading
                || groups.last().language != sub)
                groups.append({ heading, sub, {} });
            groups.last().entries.append(entry);
        }
    }
    const bool compact = list->width() < ui::px(850);
    const auto labelFont = ui::interfaceFont(chrome::labelText);
    const int actionWidth = compact
            ? qMax(ui::px(132),
                   ui::interfaceTextWidth(strings::get("dictionaries.remove"), labelFont)
                           + ui::px(48))
            : ui::px(220);
    const int nameWidth = qMax(1, list->width() - actionWidth - 3 * chrome::inset);
    const auto headerHeight = [&](const QString &text) {
        return compact
                ? qMax(int(metrics::list::headerHeight),
                       ui::wrappedInterfaceTextHeight(text, ui::interfaceFont(chrome::captionText),
                                                      list->width() - 2 * chrome::inset)
                               + ui::px(16))
                : int(metrics::list::headerHeight);
    };
    QVector<QVector<Item>> pages(1);
    int used = 0;
    QString previousSection;
    const int available =
            qMax(int(metrics::list::headerHeight + metrics::list::twoLines), list->height());
    for (const auto &group : groups) {
        bool first = true;
        for (const auto &entry : group.entries) {
            const int rowHeight = compact
                    ? qMax(int(metrics::list::twoLines),
                           ui::wrappedInterfaceTextHeight(entry.name, labelFont, nameWidth)
                                   + ui::interfaceLineHeight(ui::interfaceFont(chrome::captionText))
                                   + ui::px(36))
                    : entry.id == failedId ? int(metrics::list::twoLines)
                                           : int(metrics::list::oneLine);
            bool sectionHeader = first && previousSection != group.section;
            bool languageHeader = first && !group.language.isEmpty();
            int headers = (sectionHeader ? headerHeight(group.section) : 0)
                    + (languageHeader ? headerHeight(group.language) : 0);
            if (used && used + headers + rowHeight > available) {
                pages.append(QVector<Item>{});
                used = 0;
                sectionHeader = true;
                languageHeader = !group.language.isEmpty();
                headers = headerHeight(group.section)
                        + (languageHeader ? headerHeight(group.language) : 0);
            }
            if (sectionHeader)
                pages.last().append({ group.section, false, {}, headerHeight(group.section) });
            if (languageHeader)
                pages.last().append({ group.language, true, {}, headerHeight(group.language) });
            pages.last().append({ {}, false, entry, rowHeight });
            used += headers + rowHeight;
            first = false;
            previousSection = group.section;
        }
    }
    page = qBound(0, page, int(pages.size()) - 1);
    pager->setPage(page, pages.size(), {}, true);
    pager->show();
    int y = 0, rowIndex = 0, sectionIndex = 0, languageIndex = 0;
    for (const auto &item : pages[page]) {
        if (!item.caption.isEmpty()) {
            const auto name =
                    QString(item.language ? "rrDictionaryLanguage%1" : "rrDictionarySection%1")
                            .arg(item.language ? languageIndex++ : sectionIndex++);
            auto *header = dynamic_cast<ui::ListHeader *>(
                    list->findChild<QWidget *>(name, Qt::FindDirectChildrenOnly));
            if (!header) {
                header = new ui::ListHeader(list);
                header->setShaded(true);
                header->setObjectName(name);
            }
            retained.insert(header);
            if (header->leftLabel()->text() != item.caption)
                header->setCaptions(item.caption);
            header->leftLabel()->setWordWrap(compact);
            header->setGeometry(0, y, list->width(), item.height);
            header->show();
            y += item.height;
            continue;
        }
        const auto &entry = item.entry;
        const int index = rowIndex++;
        DictionaryLine *line = nullptr;
        for (auto *child : previous)
            if (child->property("dictionary_id").toString() == entry.id)
                line = static_cast<DictionaryLine *>(child);
        const bool created = !line;
        if (created)
            line = new DictionaryLine(list);
        retained.insert(line);
        line->setObjectName(QString("rrDictionaryRow%1").arg(index));
        line->setProperty("dictionary_id", entry.id);
        line->setGeometry(0, y, list->width(), item.height);
        auto *layout = static_cast<QHBoxLayout *>(line->layout());
        auto *row =
                created ? nullptr : static_cast<DictionaryTextRow *>(layout->itemAt(0)->widget());
        if (created) {
            layout = new QHBoxLayout(line);
            layout->setContentsMargins(0, 0, chrome::inset, 0);
            layout->setSpacing(0);
            row = new DictionaryTextRow(line);
            row->setFocusPolicy(Qt::NoFocus);
            layout->addWidget(row, 1);
        }
        row->compact = compact;
        row->setTitle(compact ? QString() : entry.name);
        const bool active = entry.id == transferId, installed = installedIds.contains(entry.id);
        row->setCaption(!compact && entry.id == failedId
                                ? strings::get("dictionaries.download_failed")
                                : QString());
        const int percent =
                totalBytes > 0 ? qBound(0, int(100.0 * receivedBytes / totalBytes), 100) : 0;
        row->setAccessory(ui::ListRow::Caption{
                active ? QString::number(percent) + "%"
                       : bytes(installed ? installedSizes.value(entry.id) : entry.size),
                ui::px(180) });
        if (compact)
            row->setAccessory(ui::ListRow::None{});
        line->name->setVisible(compact);
        line->metadata->setVisible(compact);
        line->name->setText(entry.name);
        line->name->setObjectName(QString("rrDictionaryName%1").arg(index));
        line->metadata->setObjectName(QString("rrDictionaryMetadata%1").arg(index));
        line->metadata->setText(
                entry.id == failedId ? strings::get("dictionaries.download_failed")
                        : active     ? QString::number(percent) + "%"
                                 : bytes(installed ? installedSizes.value(entry.id) : entry.size));
        const int nameHeight =
                ui::wrappedInterfaceTextHeight(entry.name, line->name->font(), nameWidth);
        line->name->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        const int metadataHeight = ui::interfaceLineHeight(line->metadata->font());
        const int textTop = qMax(0, (item.height - nameHeight - ui::px(6) - metadataHeight) / 2);
        line->name->setGeometry(chrome::inset, textTop, nameWidth, nameHeight);
        line->metadata->setGeometry(chrome::inset, line->name->geometry().bottom() + 1 + ui::px(6),
                                    nameWidth, metadataHeight);
        line->name->raise();
        line->metadata->raise();
        row->trailingLabel()->setObjectName(QString("rrDictionarySize%1").arg(index));
        auto *action = created ? nullptr : static_cast<ui::Button *>(layout->itemAt(1)->widget());
        if (created)
            action = new ui::Button(line);
        action->setText(active              ? strings::get("dictionaries.stop")
                                : installed ? strings::get("dictionaries.remove")
                                            : strings::get("dictionaries.get"));
        action->setObjectName(QString("rrDictionaryAction%1").arg(index));
        if (created) {
            layout->addWidget(action, 0, Qt::AlignVCenter);
            connect(action, &QPushButton::clicked, this, [this, id = entry.id] {
                const bool active = id == transferId, installed = installedIds.contains(id);
                failedId.clear();
                if (active) {
                    if (cancelRequested)
                        cancelRequested();
                } else if (installed) {
                    for (const auto &candidate : entries)
                        if (candidate.id == id) {
                            askConfirmation(candidate, true);
                            break;
                        }
                } else if (installRequested) {
                    for (const auto &candidate : entries)
                        if (candidate.id == id) {
                            askConfirmation(candidate, false);
                            break;
                        }
                }
                redraw();
            });
        }
        // The style's width excludes the border, so a polished button keeps the
        // same outer width as one that is not polished yet. Restyle only when
        // the width changes; restyling polishes the button again.
        if (action->property("action_width").toInt() != actionWidth) {
            ui::readerStyle(
                    action,
                    ui::styles::outlined()
                            + QString("QPushButton{font-size:%1px;min-height:%2px;max-height:%2px;min-width:%3px;max-width:%3px;padding:0;border:%4px solid palette(window-text);}")
                                      .arg(int(chrome::labelText))
                                      .arg(ui::px(66))
                                      .arg(actionWidth - 2 * int(metrics::control::border))
                                      .arg(int(metrics::control::border)));
            action->setFixedSize(actionWidth, ui::px(66));
            action->setProperty("action_width", actionWidth);
        }
        const bool download = !installed && !active;
        if (action->property("dictionary_download").toBool() != download) {
            action->setProperty("dictionary_download", download);
            if (download)
                ui::buttonIcon(action, ui::ControlIcon::Download);
            else
                action->clearIconConfiguration();
        }
        action->setIconSize(QSize(ui::px(28), ui::px(28)));
        action->setEnabled(installed || active || !working);
        layout->activate();
        if (active) {
            const auto status = unpacking ? strings::get("dictionaries.installing")
                                          : strings::get("dictionaries.downloading")
                                                    .arg(bytes(receivedBytes))
                                                    .arg(bytes(totalBytes));
            action->setToolTip(status);
            action->setAccessibleDescription(status);
            // Use the button's icon slot so progress and Stop share its
            // normal spacing and remain centered together at every text size.
            action->setIcon(
                    transferProgressIcon(percent, action->palette().color(QPalette::WindowText)));
        } else if (!download) {
            action->clearIconConfiguration();
        }
        line->show();
        y += item.height;
    }
    for (auto *child : previous) {
        if (retained.contains(child))
            continue;
        child->hide();
        child->setObjectName({});
        child->setProperty("dictionary_id", {});
        for (auto *nested : child->findChildren<QWidget *>())
            nested->setObjectName({});
        child->deleteLater();
    }
    const int listBottom = list->y() + y;
    pager->setBlankArea(
            { chrome::inset, listBottom, width() - 2 * chrome::inset, pager->y() - listBottom });
    refreshStorage();
}

void DictionariesPage::refreshStorage()
{
    qint64 usedBytes = 0;
    for (const auto &id : installedIds)
        usedBytes += installedSizes.value(id);
    QString footer = freeBytes >= 0
            ? strings::get("dictionaries.storage_free").arg(bytes(freeBytes))
            : QString();
    if (!installedIds.isEmpty())
        footer = strings::get("dictionaries.storage_used").arg(bytes(usedBytes))
                + (footer.isEmpty() ? QString() : " · " + footer);
    storage->setText(footer.toUpper());
}
} // namespace kobalt::reader
