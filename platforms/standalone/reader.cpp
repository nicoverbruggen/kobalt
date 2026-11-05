#include "reading/appearance_settings.h"
#include "reading/reading_density.h"
#include "support/strings.h"
#include "support/main_thread.h"
#include "support/diagnostics.h"
#include "reader.h"
#include "app/reader_interface.h"
#include "app/messages/opening_message.h"
#include "app/messages/recovery_message.h"
#include "reading/reading_book.h"
#include "storage/annotation_database.h"
#include "publication/native_resources.h"
#include "publication/local_resources.h"
#include <QDir>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QTimer>
#include <QUrl>
#include <utility>
#ifdef Q_OS_ANDROID
#  include <QCoreApplication>
#  include <QJniObject>
#  include <QtCore/qnativeinterface.h>
#endif

namespace kobalt::standalone {
namespace {
QString recentText(QString text)
{
    // Keep metadata and the optional cover below the 256 KiB record limit.
    text.truncate(4096);
    if (!text.isEmpty() && text.back().isHighSurrogate())
        text.chop(1);
    return text;
}
} // namespace

ReaderController::ReaderController(storage::Paths paths, EngineFontInputs fonts,
                                   QString placeholder,
                                   std::function<ReaderHostServices()> services,
                                   std::function<void(Qt::ScreenOrientation)> rotate,
                                   QObject *parent)
    : QObject(parent),
      paths(std::move(paths)),
      fonts(std::move(fonts)),
      placeholder(std::move(placeholder)),
      services(std::move(services)),
      orientation(nullptr, std::move(rotate))
{
    prepareProfile();
}

void ReaderController::prepareProfile()
{
    storage::ProfileRecovery recovery;
    storage::prepareProfile(paths, &recovery);
    storageFailed = recovery.storageFailed;
    if (recovery.recovered())
        recoveryNotice = profileRecoveryNotice(recovery, paths.directory + "/backups");
}

ReaderController::~ReaderController()
{
    Q_ASSERT(idle());
}

QJsonObject ReaderController::lastBook() const
{
    auto book = storage::readRecord(paths.directory, "last-book",
                                    paths.directory + "/last-book.json", 256 * 1024)
                        .object;
    const QFileInfo file(book.value("path").toString());
    if (!file.isFile() || !file.isReadable())
        return {};
    return book;
}

QJsonObject ReaderController::savedState(const QString &path) const
{
    // Known exception: the library reads a book's progress on the main thread.
    mainthread::AllowBlocking allow("library progress");
    const auto identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
    const auto saved = storage::readBookState(paths, identity).object;
    return saved.value("book").toString() == identity ? saved : QJsonObject();
}

QJsonObject ReaderController::preferences() const
{
    return appearance::readerPreferences(storage::readPreferences(paths.preferences()).object, {});
}

bool ReaderController::resetBook(const QString &path)
{
    if (!idle() || path.isEmpty())
        return false;
    const auto identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
    // Keep an identity-only checkpoint so the prototype fallback cannot restore
    // old settings or progress on the next opening.
    return storage::writeBookState(paths, identity, { { "book", identity } });
}

void ReaderController::open(const QString &path, QSize pixels, QString sourceKey,
                            bool restartAtStart)
{
    if (phase == Phase::Reading && path == current)
        return;
    if (!idle()) {
        pending = path;
        pendingSourceKey = std::move(sourceKey);
        pendingSize = pixels;
        pendingRestartAtStart = restartAtStart;
        retire();
        return;
    }
    this->restartAtStart = restartAtStart;
    currentSourceKey = std::move(sourceKey);
    begin(path, pixels);
}

void ReaderController::begin(const QString &path, QSize pixels)
{
    // Known exception: opening from the library prepares the profile on the
    // main thread, also while another book is still open.
    mainthread::AllowBlocking allow("library opening");
    phase = Phase::Opening;
    openingStarted.start();
    markMemoryBase();
    retirementNotified = false;
    if (storageFailed)
        prepareProfile();
    if (storageFailed) {
        emit failed(strings::get("error.profile_storage_failed"));
        retire();
        return;
    }
    if (!historyProfile && annotationDatabaseEnabled()) {
        QString error;
        historyProfile = ReadingHistory::openProfile(paths.directory, &error);
        if (!historyProfile) {
            emit failed(error == "profile-in-use" ? strings::get("error.profile_already_in_use")
                                                  : strings::get("error.saved_data_unavailable"));
            retire();
            return;
        }
    }
    current = path;
    lease = std::make_shared<SourceLease>();
    source = native::openLocalPublication(path);
    if (!source) {
        emit failed(tr("This file could not be opened as an unprotected EPUB."));
        retire();
        return;
    }
    const auto identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
    QDir().mkpath(paths.directory);
    const auto statePath = paths.bookState(identity);
    const auto stored = storage::readBookState(paths, identity);
    if (stored.status != storage::ReadStatus::Ok && stored.status != storage::ReadStatus::Missing) {
        emit failed(strings::get("error.saved_data_unavailable"));
        retire();
        return;
    }
    auto saved = stored.object;
    // Keep checkpoints from the Android prototype when the file identity
    // matches. New checkpoints use the shared identity-based storage naming.
    if (stored.status == storage::ReadStatus::Missing) {
        const auto previous = QFileInfo(paths.directory).absolutePath() + "/state/"
                + QFileInfo(path).completeBaseName() + ".json";
        const auto legacy = storage::readJsonFile(previous);
        if (legacy.object.value("book").toString() == identity) {
            const auto imported = storage::readBookState(paths, identity, previous);
            if (imported.status != storage::ReadStatus::Ok) {
                emit failed(strings::get("error.saved_data_unavailable"));
                retire();
                return;
            }
            saved = imported.object;
        }
    }
    if (saved.value("book").toString() != identity)
        saved = {};
    const auto settings = appearance::readerPreferences(
            storage::readPreferences(paths.preferences()).object, saved);
    saved["dark_mode"] = settings.value("dark_mode").toBool(false);
    saved["bold_reading"] = settings.value("bold_reading").toString("off");
    saved["avoid_wasteful_styling"] = settings.value("avoid_wasteful_styling").toBool(true);
    orientation.configure(settings);
    // The first layout leaves out the cutout bands, as the reader window does.
    QSize body = pixels;
    if (const auto safeArea = services().safeArea) {
        const auto safe = safeArea(pixels);
        body.rheight() -= safe.top + safe.bottom;
    }
    const auto viewport = readerInterface().readingViewport(body, settings);
    opening = std::make_unique<ReaderSession>(fonts, placeholder);
    QPointer<ReaderController> guard(this);
    OpenRequest request{
        identity, path, viewport, saved, {}, paths.layoutCache(), lease, source->resources(lease)
    };
#ifdef Q_OS_ANDROID
    const QJniObject context = QNativeInterface::QAndroidApplication::context();
    request.syncFilename =
            context.callObjectMethod("syncFilename", "(Ljava/lang/String;)Ljava/lang/String;",
                                     QJniObject::fromString(path).object<jstring>())
                    .toString();
    // An absent legacy name must not silently identify the internal hashed filename.
    if (request.syncFilename.isNull())
        request.syncFilename = QStringLiteral("");
#endif
    request.restartAtStart = restartAtStart;
    request.userStyleSheetPath = paths.directory + "/userstyle.css";
    request.appearanceDefaults = appearance::bookDefaults(settings);
    opening->open(
            std::move(request),
            [guard, identity, statePath, pixels](const OpeningResult &result) {
                if (!guard || guard->phase != Phase::Opening)
                    return;
                mainthread::AllowBlocking allow("library opening");
                if (!result.ready()) {
                    emit guard->failed(openingMessage(result));
                    guard->retire();
                    return;
                }
                QJsonObject last{ { "path", guard->current },
                                  { "title", recentText(guard->opening->title()) },
                                  { "author", recentText(guard->opening->author()) } };
                QByteArray cover;
                QBuffer buffer(&cover);
                buffer.open(QIODevice::WriteOnly);
                if (guard->opening->coverThumbnail().save(&buffer, "PNG")
                    && cover.size() <= 128 * 1024)
                    last["cover"] = QString::fromLatin1(cover.toBase64());
                storage::writeRecord(guard->paths.directory, "last-book",
                                     guard->paths.directory + "/last-book.json", last, 256 * 1024);
                const auto hash = guard->opening->sourceHash();
                auto book = std::make_unique<ReadingBook>(
                        std::move(guard->opening), statePath, guard->paths, identity, hash, nullptr,
                        ReadingProgress{}, ReadingProgress{}, guard->currentSourceKey);
                auto services = guard->services();
                services.phoneLayout = isPhoneScreen(pixels, readingPpi.load());
                services.startupNotice = std::exchange(guard->recoveryNotice, ReaderNotice{});
                if (services.backButtonLabel.isEmpty())
                    services.backButtonLabel = strings::get("library.exit_to_library");
                // Read the setting again: it can change while the Library is shown.
                readerInterface().setLargeText(services.textScale ? services.textScale() : 1);
                const double engine = guard->openingStarted.nsecsElapsed() / 1e6;
                QElapsedTimer creating;
                creating.start();
                auto *window = createReaderWindow(std::move(book), &guard->orientation,
                                                  std::move(services));
                window->setAttribute(Qt::WA_DeleteOnClose, false);
                window->setWindowFlags(Qt::Widget);
                window->setWindowModality(Qt::NonModal);
                window->resize(pixels);
                guard->window_ = window;
                window->finished = [guard](std::unique_ptr<ReaderSession> retained) {
                    if (guard && guard->window_)
                        guard->closing = guard->window_->closingTiming();
                    // Show the host's captured frame before this close event hides
                    // the reader and queued resource cleanup begins.
                    if (guard)
                        guard->notifyRetirement();
                    retained.reset();
                    if (guard)
                        QTimer::singleShot(0, guard, [guard] {
                            if (guard) {
                                emit guard->exitRequested();
                                guard->close();
                            }
                        });
                };
                QPointer<ReaderWindow> pendingWindow(window);
                const auto lease = guard->lease;
                const double created = creating.nsecsElapsed() / 1e6;
                QElapsedTimer waiting;
                waiting.start();
                window->preparePresentation(
                        [guard, pendingWindow, lease, engine, created, waiting](bool ready) {
                            if (!guard || !pendingWindow || guard->phase != Phase::Opening
                                || guard->window_ != pendingWindow || guard->lease != lease) {
                                return;
                            }
                            if (!ready || !lease || lease->revoked()) {
                                guard->retire();
                                return;
                            }
                            guard->phase = Phase::Reading;
                            pendingWindow->setOpeningTiming(
                                    guard->openingStarted,
                                    QJsonObject{ { "engine_ms", engine },
                                                 { "window_ms", created },
                                                 { "wait_ms", waiting.nsecsElapsed() / 1e6 } });
                            emit guard->opened();
                        });
            },
            [guard, lastStage = OpeningStage::Content,
             painted = QElapsedTimer{}](OpeningStage stage, int percent) mutable {
                if (!guard || guard->phase != Phase::Opening)
                    return;
                percent = qBound(0, percent, 100);
                // Match Kobo's progress cadence to avoid excessive e-ink redraws.
                if (stage == lastStage && percent != 100 && painted.isValid()
                    && painted.elapsed() < 200)
                    return;
                lastStage = stage;
                painted.start();
                emit guard->openingProgress((int(stage) * 100 + percent)
                                            / (int(OpeningStage::Ready) + 1));
            },
            [guard](QImage cover) {
                if (guard && guard->phase == Phase::Opening)
                    emit guard->openingCover(std::move(cover));
            });
}

void ReaderController::close()
{
    pending.clear();
    if (idle())
        emit closed();
    else
        retire();
}

void ReaderController::notifyRetirement()
{
    if (std::exchange(retirementNotified, true))
        return;
    emit retiring();
}

void ReaderController::retire()
{
    if (phase == Phase::Closing)
        return;
    const bool reading = phase == Phase::Reading;
    phase = Phase::Closing;
    notifyRetirement();
    QPointer<ReaderController> guard(this);
    if (reading && window_)
        window_->prepareShutdown([guard] {
            if (guard)
                guard->release();
        });
    else
        release();
}

void ReaderController::release()
{
    if (lease)
        lease->cancel();
    if (window_) {
        window_->finished = {};
        delete window_.data();
    }
    opening.reset();
    QPointer<ReaderController> guard(this);
    ReaderSession::drain([guard] {
        if (!guard)
            return;
        guard->source.reset();
        releaseResourceConnections([guard] {
            if (!guard)
                return;
            guard->lease.reset();
            guard->current.clear();
            guard->phase = Phase::Idle;
            reportBookClose(std::exchange(guard->closing, {}));
            if (!guard->pending.isEmpty()) {
                guard->restartAtStart = std::exchange(guard->pendingRestartAtStart, false);
                guard->currentSourceKey = std::exchange(guard->pendingSourceKey, {});
                guard->begin(std::exchange(guard->pending, {}), guard->pendingSize);
            } else
                emit guard->closed();
        });
    });
}
} // namespace kobalt::standalone
