#include "appearance_edit_session.h"
#include <QPointer>

namespace kobalt::reader {
AppearanceEditSession::~AppearanceEditSession() {
    resetPreview();
}
void AppearanceEditSession::open(ReaderSession &main, QSize viewport, std::function<void()> changed,
                                 std::function<void()> opened) {
    Q_ASSERT(!preview_);
    previewState_ = PreviewState::Opening;
    error_.clear();
    preview_ = std::make_unique<ReaderSession>(main.fontInputs(), main.placeholderFamily());
    preview_->updated = std::move(changed);
    const quint64 generation = ++generation_;
    QPointer<AppearanceEditSession> guard(this);
    preview_->openPreview(main, viewport, [guard, generation, opened = std::move(opened)](QString error) {
        if (!guard || guard->generation_ != generation)
            return;
        guard->previewState_ = error.isEmpty() ? PreviewState::Ready : PreviewState::Failed;
        guard->error_ = std::move(error);
        if (guard->ready()) {
            auto changes = std::move(guard->pendingChanges_);
            guard->pendingChanges_.clear();
            for (const auto &change : changes)
                change(*guard->preview_);
        } else
            guard->pendingChanges_.clear();
        if (opened)
            opened();
    });
}
void AppearanceEditSession::mutate(ReaderSession &main, QSize viewport, std::function<void()> changed,
                                   std::function<void()> opened, std::function<void(ReaderSession &)> operation) {
    if (previewState_ == PreviewState::Failed) {
        resetPreview();
    }
    edited_ = true;
    if (ready()) {
        operation(*preview_);
        return;
    }
    pendingChanges_.push_back(std::move(operation));
    if (!preview_)
        open(main, viewport, std::move(changed), std::move(opened));
}
void AppearanceEditSession::resetPreview() {
    ++generation_;
    pendingChanges_.clear();
    edited_ = false;
    if (preview_)
        preview_->updated = {};
    preview_.reset();
    previewState_ = PreviewState::Absent;
}
void AppearanceEditSession::discard() {
    resetPreview();
    setPanelOpen(false);
    clearDestination();
    applyState_ = ApplyState::Idle;
    retention_ = Retention::Release;
    error_.clear();
}
bool AppearanceEditSession::appearanceDestination() const {
    return destination_ == Destination::Text || destination_ == Destination::Layout;
}
bool AppearanceEditSession::externalDestination() const {
    return destination_ == Destination::Light || destination_ == Destination::Stats ||
           destination_ == Destination::Chapters || destination_ == Destination::Search ||
           destination_ == Destination::Bookmarks || destination_ == Destination::Seek;
}
AppearanceEditSession::Destination AppearanceEditSession::takeDestination() {
    const auto destination = destination_;
    clearDestination();
    return destination;
}
void AppearanceEditSession::cancelDeferredApply() {
    if (applyState_ == ApplyState::Deferred)
        applyState_ = ApplyState::Idle;
}
void AppearanceEditSession::cancelNavigation() {
    if (destination_ != Destination::Reading)
        clearDestination();
}
void AppearanceEditSession::clearContinuation() {
    cancelNavigation();
    cancelDeferredApply();
}
void AppearanceEditSession::beginApply(Retention retention) {
    retention_ = retention;
    applyState_ = ApplyState::Running;
}
void AppearanceEditSession::completeApply(bool successful, bool exiting) {
    Q_ASSERT(applying());
    applyState_ = ApplyState::Idle;
    if (successful) {
        if (!appearanceDestination())
            setPanelOpen(false);
        if (!appearanceDestination() || retention_ != Retention::Reuse)
            resetPreview();
    } else {
        cancelNavigation();
        if (exiting) {
            resetPreview();
            setPanelOpen(false);
        }
    }
    retention_ = Retention::Release;
}
}
