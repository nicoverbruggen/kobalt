#include "reader_host.h"
#include "host_fixture.h"
#include "host_services.h"
#include "interfaces/reader_interface.h"
#include "interfaces/opening_message.h"
#include "storage/reader_storage.h"
#include "reading/reading_book.h"
#include "reading/appearance_settings.h"
#include "publication/native_resources.h"
#include "publication/local_resources.h"
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

namespace kobalt::test {
ReaderHost::ReaderHost(storage::Paths paths, EngineFontInputs fonts, QString placeholderFamily, QObject *parent)
    : QObject(parent), paths(std::move(paths)), fonts(std::move(fonts)), placeholder(std::move(placeholderFamily)),
      fixtureSensor(fixtureOrientationSensor()), orientation(fixtureSensor.get(), [](Qt::ScreenOrientation) {}) {}
ReaderHost::~ReaderHost() {
    // The owner must await closed() before destruction or QApplication exit.
    Q_ASSERT(phase == Phase::Idle);
}
void ReaderHost::open(const QString &path, QSize nativeSize) {
    if (phase != Phase::Idle) {
        emit failed("Close the current book before opening another.");
        return;
    }
    phase = Phase::Opening;
    lease = std::make_shared<SourceLease>();
    source = native::openLocalPublication(path);
    if (!source) {
        phase = Phase::Idle;
        emit failed("The local EPUB resource reader could not open this file.");
        return;
    }
    const QString identity = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()).toString();
    QDir().mkpath(paths.directory);
    const QString statePath = paths.bookState(identity);
    auto saved = storage::readJson(statePath);
    if (saved.value("book").toString() != identity)
        saved = {};
    const auto settings = appearance::readerPreferences(storage::readJson(paths.preferences()), saved);
    saved["dark_mode"] = settings.value("dark_mode").toBool(false);
    saved["bold_reading"] = settings.value("bold_reading").toString("off");
    orientation.configure(settings);
    const QSize viewport = readerInterface().readingViewport(nativeSize, settings);
    opening = std::make_unique<ReaderSession>(fonts, placeholder);
    session_ = opening.get();
    QPointer<ReaderHost> guard(this);
    OpenRequest request{identity, path, viewport, saved, {}, {}, lease, source->resources(lease)};
    request.positions = positions;
    request.appearanceDefaults = appearance::bookDefaults(settings);
    opening->open(request, [guard, statePath, identity, nativeSize](const OpeningResult &outcome) {
        if (!guard || guard->phase != Phase::Opening)
            return;
        if (!outcome.ready()) {
            emit guard->failed(openingMessage(outcome));
            guard->shutdown();
            return;
        }
        const auto hash = guard->opening->sourceHash();
        const auto platform = guard->progress ? guard->progress->current : ReadingProgress{};
        auto book = std::make_unique<ReadingBook>(std::move(guard->opening), statePath, guard->paths, identity, hash,
                                                  guard->progress ? hostReadingBridge(guard->progress) : nullptr,
                                                  platform, platform);
        auto services = hostReaderServices();
        if (guard->configureServices)
            guard->configureServices(services);
        auto *window = createReaderWindow(std::move(book), &guard->orientation, std::move(services));
        window->setAttribute(Qt::WA_DeleteOnClose, false);
        window->setWindowFlags(Qt::Widget);
        window->setWindowModality(Qt::NonModal);
        window->resize(nativeSize);
        guard->window_ = window;
        guard->phase = Phase::Reading;
        window->finished = [guard](std::unique_ptr<ReaderSession> retained) {
            retained.reset();
            if (guard)
                QTimer::singleShot(0, guard, [guard] {
                    if (guard)
                        guard->shutdown();
                });
        };
        emit guard->opened();
    });
}
void ReaderHost::shutdown() {
    if (phase == Phase::Idle) {
        emit closed();
        return;
    }
    if (phase == Phase::Closing)
        return;
    const bool reading = phase == Phase::Reading;
    phase = Phase::Closing;
    QPointer<ReaderHost> guard(this);
    if (reading && window_) {
        window_->prepareShutdown([guard] {
            if (guard)
                guard->retire();
        });
    } else
        retire();
}
void ReaderHost::retire() {
    if (lease)
        lease->cancel();
    if (window_) {
        window_->finished = {};
        delete window_.data();
    }
    opening.reset();
    session_.clear();
    QPointer<ReaderHost> guard(this);
    ReaderSession::drain([guard] {
        if (!guard)
            return;
        guard->source.reset();
        releaseResourceConnections([guard] {
            if (!guard)
                return;
            guard->lease.reset();
            guard->phase = Phase::Idle;
            emit guard->closed();
        });
    });
}
}
