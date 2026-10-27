#include "captures.h"
#include "window.h"
#include "platforms/android/start_screen.h"
#include "interfaces/library/library_view.h"
#include "interfaces/reader/views/reading_page.h"
#include "build_info.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QTimer>

namespace kobalt::kindle {
void captureScreens(Window &window, const QString &book, const QString &directory) {
    struct State {
        int stage = 0, advances = 0;
        QString previousPosition;
        QElapsedTimer settled;
        QJsonArray images;
    };
    auto state = std::make_shared<State>();
    state->settled.start();
    auto *poll = new QTimer(&window);
    poll->setInterval(200);
    auto fail = [&window, poll](const QString &error) {
        qCritical().noquote() << "Kindle capture failed:" << error;
        poll->stop();
        QObject::connect(&window, &Window::bookClosed, &window, [] { QCoreApplication::exit(1); });
        if (window.idle())
            QCoreApplication::exit(1);
        else
            window.closeBook();
    };
    QObject::connect(&window, &Window::bookFailed, &window, fail);
    QObject::connect(poll, &QTimer::timeout, &window, [&, book, directory, state, poll, fail] {
        auto save = [&](const QString &name) {
            const auto image = window.grab().toImage().convertToFormat(QImage::Format_Grayscale8);
            const auto path = QDir(directory).filePath(name + ".png");
            if (image.size() != QSize(1236, 1648) || !image.save(path)) {
                fail(QStringLiteral("Cannot save a PW5 frame to %1").arg(path));
                return false;
            }
            qInfo().noquote() << "Captured" << path;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) {
                fail(QStringLiteral("Cannot checksum %1").arg(path));
                return false;
            }
            state->images.append(QJsonObject{{"file", name + ".png"}, {"width", image.width()},
                                            {"height", image.height()}, {"sha256", QString::fromLatin1(
                                                QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex())}});
            return true;
        };
        if (state->stage == 0) {
            if (!window.libraryContains(book) || state->settled.elapsed() < 800)
                return;
            if (!save("01-library")) return;
            auto *start = window.findChild<android::StartScreen *>();
            auto *search = start ? start->library()->findChild<QAbstractButton *>("librarySearch") : nullptr;
            if (!search) { fail(QStringLiteral("Library search is unavailable")); return; }
            search->click();
            state->stage = -1;
            state->settled.restart();
            return;
        }
        if (state->stage < 0) {
            if (state->settled.elapsed() < 300)
                return;
            if (state->stage == -1) {
                if (!save("05-keyboard")) return;
                auto *start = window.findChild<android::StartScreen *>();
                auto *symbols = start ? start->library()->findChild<QAbstractButton *>("widgetKeyboardSymbols") : nullptr;
                if (!symbols) { fail(QStringLiteral("Keyboard symbols are unavailable")); return; }
                symbols->click();
                state->stage = -2;
                state->settled.restart();
            } else {
                if (!save("06-keyboard-symbols")) return;
                auto *start = window.findChild<android::StartScreen *>();
                auto *close = start ? start->library()->findChild<QAbstractButton *>("libraryCloseSearch") : nullptr;
                if (!close) { fail(QStringLiteral("Library search cannot close")); return; }
                close->click();
                state->stage = 1;
                window.openBook(book);
            }
            return;
        }
        auto *reader = window.readerWindow();
        if (!reader || !reader->readyForPresentation())
            return;
        auto *page = static_cast<ReadingPage *>(reader->findChild<QWidget *>("rrPage"));
        if (!page || page->image.isNull())
            return;
        reader->checkpoint();
        const auto position = window.savedState(book).value("position").toString();
        if (position.isEmpty())
            return;
        if (state->stage == 1) {
            if (!save("02-cover")) return;
            state->previousPosition = position;
            state->stage = 2;
            reader->pageButton(1);
        } else if (state->stage == 2) {
            if (position == state->previousPosition)
                return;
            state->previousPosition = position;
            if (++state->advances < 8) {
                reader->pageButton(1);
                return;
            }
            if (!save("03-reading")) return;
            auto *settings = reader->findChild<QAbstractButton *>("rrSettings");
            if (!settings || !settings->isEnabled()) {
                fail(QStringLiteral("Reader settings button is unavailable"));
                return;
            }
            settings->click();
            state->stage = 3;
            state->settled.restart();
        } else if (state->stage == 3 && state->settled.elapsed() >= 500) {
            if (!save("04-settings")) return;
            QImage overview(1236, 576, QImage::Format_RGB32);
            overview.fill(Qt::white);
            {
                QPainter painter(&overview);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                QFont font = QApplication::font();
                font.setPixelSize(22);
                painter.setFont(font);
                const QStringList names{"01-library", "03-reading", "04-settings"};
                const QStringList captions{"Library", "Reading", "Settings"};
                for (int index = 0; index < names.size(); ++index) {
                    const int left = index * 412 + 8;
                    painter.drawText(left, 28, captions.at(index));
                    const QRect area(left, 40, 396, 528);
                    painter.drawImage(area, QImage(QDir(directory).filePath(names.at(index) + ".png")));
                    painter.setPen(Qt::gray);
                    painter.drawRect(area.adjusted(0, 0, -1, -1));
                    painter.setPen(Qt::black);
                }
            }
            QFile fixture(book);
            QSaveFile manifest(QDir(directory).filePath("manifest.json"));
            if (!overview.save(QDir(directory).filePath("overview.png")) ||
                !fixture.open(QIODevice::ReadOnly) || !manifest.open(QIODevice::WriteOnly)) {
                fail(QStringLiteral("Cannot save capture metadata"));
                return;
            }
            const QJsonDocument metadata(QJsonObject{{"kind", "kobalt-kindle-screenshots"}, {"qt", qVersion()},
                {"commit", kobaltCommit}, {"modified", bool(KOBALT_COMMIT_MODIFIED)}, {"width", 1236}, {"height", 1648},
                {"fixture_sha256", QString::fromLatin1(QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex())},
                {"images", state->images}});
            const auto bytes = metadata.toJson();
            if (manifest.write(bytes) != bytes.size() || !manifest.commit()) {
                fail(QStringLiteral("Cannot commit capture metadata"));
                return;
            }
            poll->stop();
            QObject::connect(&window, &Window::bookClosed, &window, [&window] { window.close(); });
            window.closeBook();
        }
    });
    QTimer::singleShot(60000, poll, [fail, poll] {
        if (poll->isActive()) fail(QStringLiteral("60 second timeout"));
    });
    poll->start();
}
}
