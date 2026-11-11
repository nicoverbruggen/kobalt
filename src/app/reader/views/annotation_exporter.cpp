#include "annotation_exporter.h"
#include "support/save_wait.h"
#include "storage/record_writer.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include "support/strings.h"
#include "ui/dialogs/modals.h"
#include "ui/style/metrics.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <utility>

namespace kobalt::reader {
namespace {
class QtAnnotationExporter final : public QObject, public AnnotationExporter
{
public:
    QtAnnotationExporter(QWidget *reader, QWidget *nativeParent, QString directory, bool usePicker,
                         ExportWriter write)
        : reader(reader),
          nativeParent(nativeParent),
          directory(std::move(directory)),
          usePicker(usePicker),
          write(std::move(write))
    {
        if (reader)
            connect(reader, &QObject::destroyed, this, [this] { cancel(); });
    }

    ~QtAnnotationExporter() override
    {
        alive.reset();
        cancel();
    }

    void start(AnnotationExportRequest next, Done completed) override
    {
        if (active())
            return;
        request = std::move(next);
        done = std::move(completed);
        cancelled = false;
        if (!reader || request.fileName.isEmpty() || (!usePicker && directory.isEmpty())) {
            finish({ AnnotationExportStatus::Failed, {} });
            return;
        }
        const auto path = usePicker ? QString() : QDir(directory).filePath(request.fileName);
        confirm(path, !usePicker && exists(path));
    }

    void cancel() override
    {
        cancelled = true;
        done = {};
        request = {};
        closeDialog();
    }

    bool active() const override { return currentDialog || writePending; }

    bool writing() const override { return writePending; }

    QWidget *dialog() const override { return currentDialog; }

private:
    static bool exists(const QString &path)
    {
        const QFileInfo file(path);
        return file.exists() || file.isSymLink();
    }

    void closeDialog()
    {
        const auto dialog = currentDialog;
        currentDialog.clear();
        if (dialog) {
            if (auto *modal = dynamic_cast<ui::ModalOverlay *>(dialog.data()))
                modal->dismissed = {};
            QObject::disconnect(dialog, nullptr, this, nullptr);
            dialog->hide();
            dialog->deleteLater();
        }
    }

    void finish(AnnotationExportResult result)
    {
        closeDialog();
        request = {};
        const auto completed = std::move(done);
        done = {};
        if (!cancelled && completed)
            completed(std::move(result));
    }

    void confirm(const QString &path, bool replace)
    {
        if (!reader || cancelled)
            return;
        auto description =
                strings::get(replace ? "annotations.export_replace_description"
                                     : "annotations.export_description")
                        .arg(request.title.isEmpty() ? strings::get("annotations.export_untitled")
                                                     : request.title,
                             request.formatName);
        if (!path.isEmpty())
            description += "\n\n" + strings::get("annotations.export_destination").arg(path);
        auto *dialog = new ui::ChoiceModal(
                reader,
                { description,
                  strings::get(replace ? "annotations.export_replace_title"
                                       : "annotations.export_title"),
                  {} },
                { strings::get("common.cancel"), "rrAnnotationExportCancel" },
                { strings::get(replace ? "annotations.export_replace_action"
                                       : "annotations.export_action"),
                  replace ? "rrAnnotationExportReplace" : "rrAnnotationExportConfirm" });
        for (auto *label : dialog->findChildren<QLabel *>())
            label->setTextFormat(Qt::PlainText);
        currentDialog = dialog;
        dialog->setObjectName(replace ? "rrAnnotationExportReplaceDialog"
                                      : "rrAnnotationExportDialog");
        const auto reject = [this] { finish({ AnnotationExportStatus::Cancelled, {} }); };
        dialog->dismissed = reject;
        connect(dialog->secondaryButton(), &QPushButton::clicked, this, reject);
        connect(dialog->primaryButton(), &QPushButton::clicked, this, [this, path, replace] {
            closeDialog();
            if (!request.mayConfirm || !request.mayConfirm()) {
                finish({ AnnotationExportStatus::Cancelled, {} });
                return;
            }
            const auto bytes = request.archive ? request.archive() : QByteArray();
            if (bytes.isEmpty()) {
                finish({ AnnotationExportStatus::Failed, {} });
                return;
            }
            if (usePicker)
                choose(bytes);
            else
                save(QUrl::fromLocalFile(path), bytes, replace);
        });
        namespace metrics = ui::metrics;
        dialog->card()->fitToWidth(qMin<int>(metrics::notice::cardWidth,
                                             reader->width() - 2 * metrics::notice::sideClearance));
        dialog->centerCard();
        dialog->show();
        dialog->raise();
    }

    void choose(const QByteArray &bytes)
    {
        auto *picker = new QFileDialog(
                nativeParent ? nativeParent.data() : reader.data(),
                strings::get("annotations.export_format_title").arg(request.formatName),
                request.fileName, strings::get("annotations.export_filter"));
        currentDialog = picker;
        picker->setObjectName("rrAnnotationExportPicker");
        picker->setAcceptMode(QFileDialog::AcceptSave);
        picker->setFileMode(QFileDialog::AnyFile);
        picker->setDefaultSuffix("zip");
        picker->setOption(QFileDialog::DontConfirmOverwrite, false);
        connect(picker, &QDialog::finished, this, [this, picker, bytes](int result) {
            const auto destination = picker->selectedUrls().value(0);
            closeDialog();
            if (result != QDialog::Accepted || !request.mayWrite || !request.mayWrite()) {
                finish({ AnnotationExportStatus::Cancelled, {} });
                return;
            }
            // The native save picker owns its overwrite confirmation.
            save(destination, bytes, true);
        });
        picker->QDialog::open();
    }

    void save(const QUrl &destination, const QByteArray &bytes, bool replace)
    {
        writePending = true;
        auto needsConsent = std::make_shared<bool>(false);
        const auto path = destination.isLocalFile() ? destination.toLocalFile() : QString();
        const auto createDirectory = !usePicker;
        const std::weak_ptr<QtAnnotationExporter *> guard = alive;
        writer.post(
                [destination, path, bytes, replace, needsConsent, createDirectory, write = write] {
                    mainthread::blocking("annotation-export");
                    if (createDirectory && !QDir().mkpath(QFileInfo(path).absolutePath()))
                        return false;
                    if (!replace && exists(path)) {
                        *needsConsent = true;
                        return false;
                    }
                    return write && write(destination, bytes);
                },
                [guard, needsConsent, path](bool saved) {
                    qt::invoke(
                            QCoreApplication::instance(),
                            [guard, needsConsent, path, saved] {
                                if (const auto exporter = guard.lock())
                                    (*exporter)->written(saved, *needsConsent, path);
                            },
                            Qt::QueuedConnection);
                });
    }

    void written(bool saved, bool needsConsent, const QString &path)
    {
        // The callback can reach the GUI before RecordWriter clears running.
        // Never destroy the writer or release storage until that barrier passes.
        if (!writer.idle()) {
            saveWait.after(
                    [this, saved, needsConsent, path] { written(saved, needsConsent, path); });
            return;
        }
        writePending = false;
        saves::settled();
        if (cancelled)
            return;
        if (needsConsent) {
            if (request.mayConfirm && request.mayConfirm())
                confirm(path, true);
            else
                finish({ AnnotationExportStatus::Cancelled, {} });
            return;
        }
        finish({ saved ? AnnotationExportStatus::Saved : AnnotationExportStatus::Failed, path });
    }

    QPointer<QWidget> reader, nativeParent, currentDialog;
    QString directory;
    bool usePicker, cancelled = false, writePending = false;
    // The writer thread reports when its write no longer runs.
    SaveWait saveWait{ this };
    ExportWriter write;
    AnnotationExportRequest request;
    Done done;
    storage::RecordWriter writer;
    std::shared_ptr<QtAnnotationExporter *> alive = std::make_shared<QtAnnotationExporter *>(this);
};
} // namespace

std::unique_ptr<AnnotationExporter> directoryAnnotationExporter(QWidget *reader, QString directory,
                                                                ExportWriter write)
{
    return std::make_unique<QtAnnotationExporter>(reader, nullptr, std::move(directory), false,
                                                  std::move(write));
}

std::unique_ptr<AnnotationExporter> pickerAnnotationExporter(QWidget *reader, QWidget *nativeParent,
                                                             ExportWriter write)
{
    return std::make_unique<QtAnnotationExporter>(reader, nativeParent, QString(), true,
                                                  std::move(write));
}
} // namespace kobalt::reader
