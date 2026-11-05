#pragma once
#include "reading/reader_session.h"
#include <QObject>
#include <utility>
#include <vector>

namespace kobalt::reader {
// The reader owns widgets and persistence. This owner holds one preview and
// the continuation that follows a successful apply to the main session.
class AppearanceEditSession final : public QObject {
public:
    enum class PreviewState { Absent, Opening, Ready, Failed };
    enum class ApplyState { Idle, Deferred, Running };
    enum class Destination { None, Controls, Reading, Text, Layout, Light, Stats, Chapters, Search, Bookmarks, Seek };
    enum class Retention { Release, Reuse };
    AppearanceEditSession() = default;
    ~AppearanceEditSession() override;
    AppearanceEditSession(const AppearanceEditSession &) = delete;
    AppearanceEditSession &operator=(const AppearanceEditSession &) = delete;

    ReaderSession *preview() const {
        return preview_.get();
    }
    bool ready() const {
        return previewState_ == PreviewState::Ready;
    }
    bool panelOpen() const {
        return panelOpen_;
    }
    void setPanelOpen(bool open) {
        if (panelOpen_ != open)
            ++panelGeneration_;
        panelOpen_ = open;
    }
    quint64 panelGeneration() const {
        return panelGeneration_;
    }
    bool edited() const {
        return edited_;
    }
    // Edits made before the preview opens wait in its queue. Until then, the
    // controls hold the latest requested values and no session has them.
    bool queued() const {
        return edited_ && !ready();
    }
    PreviewState previewState() const {
        return previewState_;
    }
    ApplyState applyState() const {
        return applyState_;
    }
    bool applying() const {
        return applyState_ == ApplyState::Running;
    }
    bool finishPending() const {
        return applyState_ == ApplyState::Deferred;
    }
    const QString &error() const {
        return error_;
    }
    void setError(QString error) {
        error_ = std::move(error);
    }
    void clearError() {
        error_.clear();
    }
    void open(ReaderSession &main, QSize viewport, std::function<void()> changed, std::function<void()> opened);
    void mutate(ReaderSession &main, QSize viewport, std::function<void()> changed, std::function<void()> opened,
                std::function<void(ReaderSession &)> operation);
    void resetPreview();
    void discard();

    Destination destination() const {
        return destination_;
    }
    void request(Destination destination) {
        destination_ = destination;
    }
    void clearDestination() {
        destination_ = Destination::None;
    }
    void cancelNavigation();
    bool appearanceDestination() const;
    bool externalDestination() const;
    bool panelDestination() const {
        return appearanceDestination() || externalDestination();
    }
    int panelIndex() const {
        return destination_ == Destination::Text ? 0 : 1;
    }
    Destination takeDestination();
    // Cancelling panel navigation retains an unanswered request to return
    // to reading. Sleep/hide also cancels a deferred apply, but an already
    // running main apply still completes.
    void clearContinuation();
    void deferApply() {
        applyState_ = ApplyState::Deferred;
    }
    void cancelDeferredApply();
    void beginApply(Retention retention = Retention::Release);
    void completeApply(bool successful, bool exiting);

private:
    std::unique_ptr<ReaderSession> preview_;
    PreviewState previewState_ = PreviewState::Absent;
    ApplyState applyState_ = ApplyState::Idle;
    Destination destination_ = Destination::None;
    Retention retention_ = Retention::Release;
    QString error_;
    quint64 generation_ = 0;
    bool panelOpen_ = false;
    quint64 panelGeneration_ = 0;
    bool edited_ = false;
    std::vector<std::function<void(ReaderSession &)>> pendingChanges_;
};
}
