#include "support/strings.h"
#include "reading_book.h"
#include "appearance_settings.h"
#include "reader_session.h"
#include "storage/reader_storage.h"
#include "support/diagnostics.h"

namespace kobalt {
ReadingBook::ReadingBook(std::unique_ptr<ReaderSession> session, QString statePath, storage::Paths paths,
                         QString identity, QByteArray digest, std::unique_ptr<ReadingBridge> bridge,
                         ReadingProgress baseline, ReadingProgress diskBaseline)
    : model(std::move(session)), statePath(std::move(statePath)), identity_(std::move(identity)),
      paths_(std::move(paths)), digest(std::move(digest)), bridge(std::move(bridge)), baseline(std::move(baseline)),
      reconciliation(std::move(diskBaseline)) {
    const auto saved = storage::readJson(this->statePath);
    const auto preferences = appearance::readerPreferences(
        storage::readJson(paths_.preferences()), saved.value("book").toString() == identity_ ? saved : QJsonObject());
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            layoutPreferences_[key] = preferences.value(key);
    if (saved.value("book").toString() == identity_)
        dictionaryChoice_ = saved.value("dictionary_id").toString();
    // Qt 5.2 leaves a default-constructed QElapsedTimer uninitialized, so
    // isValid() is not reliable until the timer is invalidated or started.
    progressPoll.invalidate();
    activeReading.invalidate();
}
ReadingBook::~ReadingBook() = default;
bool ReadingBook::deviceTransition() const {
    return bridge && bridge->deviceTransition();
}
bool ReadingBook::canWriteNative() const {
    return bridge && bridge->writable();
}
void ReadingBook::releaseNative() {
    stopReading();
    bridge.reset();
    released = true;
}
void ReadingBook::observeReading() {
    if (released || (bridge && !bridge->writable())) {
        stopReading();
        return;
    }
    if (model->busy())
        return;
    if (model->image().isNull() || !model->error().isEmpty()) {
        stopReading();
        return;
    }
    if (!activeReading.isValid())
        activeReading.start();
    if (bridge)
        bridge->observeReading(readingActivity());
}
ReadingActivity ReadingBook::readingActivity() const {
    return {model->state().value("position").toString(),
            100.0 * model->page() / qMax(1, model->pages()),
            model->page(),
            model->pages(),
            model->turnDirection(),
            model->totalWords(),
            model->chapterGraph(),
            model->graphSelection()};
}
ReadingEstimates ReadingBook::estimates() const {
    if (bridge && !released && bridge->writable()) {
        ReadingEstimates result;
        if (bridge->estimates(readingActivity(), &result))
            return result;
    }
    const auto reading = readingMilliseconds();
    const int chapterLeft = model->chapterPages() > 0 ? model->chapterPages() - model->chapterPage() + 1 : -1;
    return {pace_.estimate(chapterLeft, reading), pace_.estimate(model->pages() - model->page(), reading),
            pace_.estimate(0, reading) < 0};
}
void ReadingBook::stopReading() {
    if (activeReading.isValid()) {
        completedReadingMilliseconds += activeReading.elapsed();
        activeReading.invalidate();
    }
    if (bridge)
        bridge->stopReading();
}
qint64 ReadingBook::readingMilliseconds() const {
    return completedReadingMilliseconds + (activeReading.isValid() ? activeReading.elapsed() : 0);
}
bool ReadingBook::offerFinish() const {
    if (released || (bridge && !bridge->writable()) || model->busy() || model->pages() <= 0 ||
        !model->error().isEmpty())
        return false;
    const auto anchor = model->state().value("position").toString();
    if (anchor.isEmpty() || anchor == acceptedFinishedAnchor)
        return false;
    // Nickel asks on exit at 90%. Include the final page of very short books,
    // and the last page of the text before trailing non-linear sections.
    return model->page() * 10 >= model->pages() * 9 || model->nextPage() == model->page();
}
void ReadingBook::markFinished() {
    if (offerFinish())
        acceptedFinishedAnchor = model->state().value("position").toString();
}
ReadingProgress ReadingBook::readProgress() const {
    return bridge ? bridge->read() : ReadingProgress{};
}
void ReadingBook::observeProgress() {
    if (bridge)
        reconciliation.observe(bridge->read());
}
void ReadingBook::pollProgress() {
    if (!bridge)
        return;
    // Nickel's saved position changes after sync, account updates, USB and
    // wake. Check then, and otherwise once a minute.
    if (!bridge->takeChange() && progressPoll.isValid() && progressPoll.elapsed() < 60000)
        return;
    observeProgress();
    progressPoll.restart();
    if (!reconciliation.pending() && reconciliation.baseline().valid)
        baseline = reconciliation.baseline();
}
void ReadingBook::acknowledge(const ReadingProgress &shown, bool moved) {
    reconciliation.acknowledge(shown);
    baseline = reconciliation.baseline();
    acceptedFinishedAnchor = moved && shown.status == 2 ? model->state().value("position").toString() : QString();
}
bool ReadingBook::writeNative(const QString &location, const QString &mappingError) {
    if (!bridge && !released) {
        nativeWriteback = "local-only";
        nativeWritebackError.clear();
        return true;
    }
    // Some plain EPUB files have positions Nickel cannot store. Record the
    // time, status and percentage; Nickel keeps its own position. Kobo's cloud
    // does not sync sideloaded EPUB files, so no other device sees the old
    // position with a new time. Other books save only locally when their
    // position cannot be mapped.
    const bool timeOnly = !mappingError.isEmpty();
    QString error = timeOnly && !plainEpub() ? mappingError : QString();
    nativeWriteback = "local-only";
    if (error.isEmpty() && canWriteNative()) {
        const auto current = bridge->read(&error);
        reconciliation.observe(current);
        if (reconciliation.pending())
            return false;
        if (reconciliation.mayWrite(current)) {
            ReadingProgress verified;
            const double percent = 100.0 * model->page() / qMax(1, model->pages());
            const bool preserveFinished = !acceptedFinishedAnchor.isEmpty() &&
                                          acceptedFinishedAnchor == model->state().value("position").toString();
            diagnostic("native-save-start");
            if (bridge->write(current, timeOnly ? QString() : location, percent, preserveFinished, readingActivity(),
                              &verified, &error)) {
                baseline = verified;
                reconciliation = ProgressReconciler(verified);
                nativeWriteback = timeOnly ? "time-only" : "verified";
            } else {
                // A commit can lose a race after the earlier read. Ask about
                // that update before letting the window finish its exit.
                observeProgress();
                if (reconciliation.pending() && canWriteNative())
                    return false;
            }
        } else if (error.isEmpty())
            error = strings::get("error.nickel_progress_changed_before_exit");
    } else if (error.isEmpty())
        error = strings::get("error.nickel_save_deferred_while_progress_or_device_state_is_unresolved");
    nativeWritebackError = error;
    diagnostic("native-save",
               {{"result", nativeWriteback}, {"mapping_failed", timeOnly}, {"failed", !error.isEmpty()}});
    return true;
}
bool ReadingBook::saveDictionaryChoice(const QString &id) {
    const auto previous = dictionaryChoice_;
    dictionaryChoice_ = id;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    dictionaryChoice_ = previous;
    return false;
}
bool ReadingBook::saveLayoutPreferences(const QJsonObject &preferences) {
    QJsonObject next;
    for (const auto *key : appearance::layoutKeys)
        if (preferences.contains(key))
            next[key] = preferences.value(key);
    const auto previous = layoutPreferences_;
    layoutPreferences_ = next;
    QJsonObject snapshot;
    if (checkpoint(&snapshot))
        return true;
    layoutPreferences_ = previous;
    return false;
}
bool ReadingBook::checkpoint(QJsonObject *snapshot) {
    auto state = model->state();
    state["book"] = identity_;
    state["fingerprint"] = QString::fromLatin1(digest);
    for (auto it = layoutPreferences_.begin(); it != layoutPreferences_.end(); ++it)
        state[it.key()] = it.value();
    if (!dictionaryChoice_.isEmpty())
        state["dictionary_id"] = dictionaryChoice_;
    state["schema"] = 1;
    state["nickel_progress"] = baseline.json();
    state["finished_position"] = acceptedFinishedAnchor;
    state["native_writeback"] = nativeWriteback;
    state["sync_pending"] = reconciliation.pending();
    state["native_writeback_error"] = nativeWritebackError;
    *snapshot = state;
    auto persisted = state;
    if (model->temporaryTypography())
        appearance::copy(persisted, model->persistentState());
    if (persisted == savedCheckpoint)
        return true;
    const bool saved = storage::writeJson(statePath, persisted);
    if (saved) {
        savedCheckpoint = persisted;
        if (!model->temporaryTypography()) {
            const auto settings = storage::readJsonFile(paths_.preferences());
            if (settings.status == storage::ReadStatus::Ok || settings.status == storage::ReadStatus::Missing) {
                auto next = settings.object;
                appearance::rememberTextLayout(next, state);
                if (next != settings.object && !storage::writeJson(paths_.preferences(), next)) {
                    diagnostic("last-text-layout-unsaved", {}, false);
                }
            }
        }
    } else {
        diagnostic("local-save-failed");
    }
    return saved;
}
}
