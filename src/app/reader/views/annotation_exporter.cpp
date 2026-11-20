#include "annotation_exporter.h"
#include "support/save_wait.h"
#include "support/main_thread.h"
#include "support/strings.h"
#include "ui/dialogs/modals.h"
#include <QThread>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMutex>
#include <QPointer>
#include <QPushButton>
#include <utility>

namespace kobalt::reader {
namespace {
bool destinationExists(const QString &path)
{
    const QFileInfo file(path);
    return file.exists() || file.isSymLink();
}

class DestinationWrite final : public QThread
{
public:
    using QThread::QThread;
    ExportWriter write;
    QUrl destination;
    QByteArray bytes;
    bool createDirectory = false, replace = false;
    bool saved = false, needsConsent = false;

    void run() override
    {
        mainthread::blocking("annotation-export");
        // Two readers can confirm the same new path before either write starts.
        // Keep the destination check and write together across exports.
        static QMutex destinations;
        QMutexLocker lock(&destinations);
        const auto path = destination.toLocalFile();
        if (createDirectory && !QDir().mkpath(QFileInfo(path).absolutePath()))
            return;
        if (!replace && destinationExists(path)) {
            needsConsent = true;
            return;
        }
        try {
            saved = write && write(destination, bytes);
        } catch (...) {
            saved = false;
        }
    }
};

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
        cancel();
        if (writer) {
            mainthread::blocking("annotation-export-close");
            writer->wait();
        }
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
        confirm(path, !usePicker && destinationExists(path));
    }

    void cancel() override
    {
        cancelled = true;
        done = {};
        request = {};
        closeDialog();
    }

    bool active() const override { return currentDialog || writer; }

    bool writing() const override { return writer != nullptr; }

    QWidget *dialog() const override { return currentDialog; }

private:
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
        auto *job = new DestinationWrite(this);
        job->write = write;
        job->destination = destination;
        job->bytes = bytes;
        job->replace = replace;
        job->createDirectory = !usePicker;
        writer = job;
        connect(
                job, &QThread::finished, this,
                [this, job] {
                    writer = nullptr;
                    job->deleteLater();
                    written(job->saved, job->needsConsent, job->destination.toLocalFile());
                },
                Qt::QueuedConnection);
        job->start();
    }

    void written(bool saved, bool needsConsent, const QString &path)
    {
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
    bool usePicker, cancelled = false;
    ExportWriter write;
    AnnotationExportRequest request;
    Done done;
    DestinationWrite *writer = nullptr;
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
