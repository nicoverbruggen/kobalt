#pragma once
#include <QByteArray>
#include <QString>
#include <functional>
#include <memory>
class QWidget;

namespace kobalt {
enum class AnnotationExportStatus { Saved, Cancelled, Failed, Unsupported };

struct AnnotationExportResult
{
    AnnotationExportStatus status = AnnotationExportStatus::Failed;
    QString location;
};

struct AnnotationExportRequest
{
    QString fileName, title, formatName;
    // Called on the GUI thread, after consent. The archive remains shared code.
    std::function<QByteArray()> archive;
    std::function<bool()> mayConfirm, mayWrite;
};

// One operation belongs to an open reader. The host owns destination selection,
// overwrite consent and writing. Callbacks run on the GUI thread. Cancellation
// dismisses UI and drops completion observers; it does not undo a started write.
// Storage handoffs must wait for writing() to become false. Destruction drains
// any remaining write and is only allowed at a blocking lifecycle boundary.
class AnnotationExporter
{
public:
    using Done = std::function<void(AnnotationExportResult)>;
    virtual ~AnnotationExporter() = default;
    virtual void start(AnnotationExportRequest request, Done done) = 0;
    virtual void cancel() = 0;
    virtual bool active() const = 0;
    virtual bool writing() const = 0;
    virtual QWidget *dialog() const = 0;
};

using AnnotationExporterFactory = std::function<std::unique_ptr<AnnotationExporter>(QWidget *)>;
} // namespace kobalt
