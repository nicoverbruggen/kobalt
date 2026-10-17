#include "reading/appearance_settings.h"
#include "reader.h"
#include "interfaces/reader_interface.h"
#include "interfaces/opening_message.h"
#include "reading/reading_book.h"
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

namespace kobalt::android {
namespace {
QString recentText(QString text) {
    // Keep metadata and the optional cover below the 256 KiB record limit.
    text.truncate(4096);
    if (!text.isEmpty() && text.back().isHighSurrogate())
        text.chop(1);
    return text;
}
}

ReaderController::ReaderController(storage::Paths paths, EngineFontInputs fonts, QString placeholder,
                                   std::function<ReaderHostServices()> services,
                                   std::function<void(Qt::ScreenOrientation)> rotate, QObject *parent)
    : QObject(parent), paths(std::move(paths)), fonts(std::move(fonts)), placeholder(std::move(placeholder)),
      services(std::move(services)), orientation(nullptr, std::move(rotate)) {}
ReaderController::~ReaderController() {
    Q_ASSERT(idle());
}
QJsonObject ReaderController::lastBook() const {
    auto book = storage::readJsonFile(paths.directory + "/last-book.json", 256 * 1024).object;
    const QFileInfo file(book.value("path").toString());
    if (!file.isFile() || !file.isReadable())
        return {};
    return book;
}
bool ReaderController::forgetLastBook() {
    const auto file = paths.directory + "/last-book.json";
    return !QFile::exists(file) || QFile::remove(file);
}
void ReaderController::open(const QString &path, QSize pixels) {
    if (phase == Phase::Reading && path == current)
        return;
    if (!idle()) {
        pending = path;
        pendingSize = pixels;
        retire();
        return;
    }
    begin(path, pixels);
}
void ReaderController::begin(const QString &path, QSize pixels) {
    phase = Phase::Opening;
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
    auto saved = storage::readJson(statePath);
    // Keep checkpoints from the Android prototype when the file identity
    // matches. New checkpoints use the shared identity-based storage naming.
    if (saved.isEmpty()) {
        const auto previous =
            QFileInfo(paths.directory).absolutePath() + "/state/" + QFileInfo(path).completeBaseName() + ".json";
        saved = storage::readJson(previous);
    }
    if (saved.value("book").toString() != identity)
        saved = {};
    const auto settings = appearance::readerPreferences(storage::readJson(paths.preferences()), saved);
    saved["dark_mode"] = settings.value("dark_mode").toBool(false);
    saved["bold_reading"] = settings.value("bold_reading").toString("off");
    orientation.configure(settings);
    const auto viewport = readerInterface().readingViewport(pixels, settings);
    opening = std::make_unique<ReaderSession>(fonts, placeholder);
    QPointer<ReaderController> guard(this);
    OpenRequest request{identity, path, viewport, saved, {}, paths.layoutCache(), lease, source->resources(lease)};
    request.userStyleSheetPath = paths.directory + "/userstyle.css";
    request.appearanceDefaults = appearance::bookDefaults(settings);
    opening->open(
        std::move(request),
        [guard, identity, statePath, pixels](const OpeningResult &result) {
            if (!guard || guard->phase != Phase::Opening)
                return;
            if (!result.ready()) {
                emit guard->failed(openingMessage(result));
                guard->retire();
                return;
            }
            QJsonObject last{{"path", guard->current},
                             {"title", recentText(guard->opening->title())},
                             {"author", recentText(guard->opening->author())}};
            QByteArray cover;
            QBuffer buffer(&cover);
            buffer.open(QIODevice::WriteOnly);
            if (guard->opening->coverThumbnail().save(&buffer, "PNG") && cover.size() <= 128 * 1024)
                last["cover"] = QString::fromLatin1(cover.toBase64());
            storage::writeJson(guard->paths.directory + "/last-book.json", last);
            const auto hash = guard->opening->sourceHash();
            auto book = std::make_unique<ReadingBook>(std::move(guard->opening), statePath, guard->paths, identity,
                                                      hash, nullptr, ReadingProgress{}, ReadingProgress{});
            auto *window = createReaderWindow(std::move(book), &guard->orientation, guard->services());
            window->setAttribute(Qt::WA_DeleteOnClose, false);
            window->setWindowFlags(Qt::Widget);
            window->setWindowModality(Qt::NonModal);
            window->resize(pixels);
            guard->window_ = window;
            guard->phase = Phase::Reading;
            window->finished = [guard](std::unique_ptr<ReaderSession> retained) {
                retained.reset();
                if (guard)
                    QTimer::singleShot(0, guard, [guard] {
                        if (guard) {
                            emit guard->exitRequested();
                            guard->close();
                        }
                    });
            };
            emit guard->opened();
        },
        [guard, lastStage = OpeningStage::Content, painted = QElapsedTimer{}](OpeningStage stage, int percent) mutable {
            if (!guard || guard->phase != Phase::Opening)
                return;
            percent = qBound(0, percent, 100);
            // Match Kobo's progress cadence to avoid excessive e-ink redraws.
            if (stage == lastStage && percent != 100 && painted.isValid() && painted.elapsed() < 200)
                return;
            lastStage = stage;
            painted.start();
            emit guard->openingProgress((int(stage) * 100 + percent) / (int(OpeningStage::Ready) + 1));
        });
}
void ReaderController::close() {
    pending.clear();
    if (idle())
        emit closed();
    else
        retire();
}
void ReaderController::retire() {
    if (phase == Phase::Closing)
        return;
    const bool reading = phase == Phase::Reading;
    phase = Phase::Closing;
    QPointer<ReaderController> guard(this);
    if (reading && window_)
        window_->prepareShutdown([guard] {
            if (guard)
                guard->release();
        });
    else
        release();
}
void ReaderController::release() {
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
            if (!guard->pending.isEmpty())
                guard->begin(std::exchange(guard->pending, {}), guard->pendingSize);
            else
                emit guard->closed();
        });
    });
}
}
