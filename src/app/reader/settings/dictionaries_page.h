#pragma once
#include "ui/controls/paging.h"
#include "ui/dialogs/modals.h"
#include "dictionaries/manager.h"
#include <QWidget>
#include <QSet>
#include <QHash>
#include <QPointer>
class QLabel;

namespace kobalt::ui {
class PagerBar;
class ChoiceModal;
} // namespace kobalt::ui

namespace kobalt::reader {
class DictionariesPage final : public QWidget
{
public:
    explicit DictionariesPage(QWidget *parent);
    ~DictionariesPage() override;
    int preferredHeight() const;
    void setLanguage(const QString &language);
    void setCatalog(const QVector<DownloadDictionary> &entries);
    void setState(const QSet<QString> &installed, bool busy, const QString &status);
    void setStorage(const QHash<QString, qint64> &sizes, qint64 freeBytes,
                    const QString &selectedId = {});
    void setTransfer(const QString &id, qint64 received, qint64 total, bool installing);
    std::function<void(const QString &)> installRequested, removeRequested;
    std::function<void()> cancelRequested;

protected:
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void redraw();
    void refreshStorage();
    void dismissConfirmation();
    void askConfirmation(const DownloadDictionary &entry, bool removing);
    QVector<DownloadDictionary> entries;
    QSet<QString> installedIds;
    QHash<QString, qint64> installedSizes;
    QString language, transferId, endedId, failedId, requestedId;
    qint64 freeBytes = -1, receivedBytes = 0, totalBytes = 0;
    bool working = false, unpacking = false;
    int page = 0;
    QWidget *list;
    QLabel *storage;
    ui::PagerBar *pager;
    QPointer<ui::ChoiceModal> confirmation;
};
} // namespace kobalt::reader
