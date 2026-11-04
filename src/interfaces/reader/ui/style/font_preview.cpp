#include "font_preview.h"
#include "private_text.h"
#include "support/diagnostics.h"
#include "support/main_thread.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QCache>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QPointer>
#include <QPicture>
#include <QVariant>
#include <QRunnable>
#include <QThreadPool>
#include <deque>

namespace kobalt::ui {
namespace {
QString identity(const QStringList &files) {
    QString result;
    for (const auto &path : files) {
        const QFileInfo file(path);
        result += path + '\n' + QString::number(file.size()) + ':' +
                  QString::number(file.lastModified().toMSecsSinceEpoch()) + '\n';
    }
    return result;
}
bool preparing(QWidget *widget) {
    for (auto *parent = widget; parent; parent = parent->parentWidget())
        if (parent->property("font_preview_preparing").toBool()) return true;
    return false;
}
struct Request {
    QPointer<QWidget> owner;
    QString key, text;
    FontPreviewSource source;
    QSize size;
    int inkHeight, weight;
    bool boldReading, centered, wrap;
};
class Task : public QRunnable {
public:
    std::function<void()> work;
    void run() override { work(); }
};
class Previews : public QObject {
public:
    Previews() : QObject(qApp), cache(fontPreviewBitmapBudget) { pool.setMaxThreadCount(1); }
    ~Previews() override {
        // QApplication is shutting down; no reader input can arrive now.
        mainthread::AllowBlocking shutdown("font preview worker shutdown");
        mainthread::blocking("font-preview-worker-stop");
        pool.waitForDone();
    }
    bool eventFilter(QObject *object, QEvent *event) override {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::KeyPress:
        case QEvent::KeyRelease:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
            // The old page may still be on screen. Do not select a new row
            // using coordinates from that old page while its replacement loads.
            if (preparing(qobject_cast<QWidget *>(object))) return true;
            break;
        default: break;
        }
        return QObject::eventFilter(object, event);
    }
    QCache<QString, QImage> cache;
    std::deque<Request> pending;
    bool busy = false;
    void request(Request request) {
        request.owner->setProperty("font_preview_key", request.key);
        request.owner->setProperty("font_preview_ready", false);
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->owner == request.owner) {
                if (it->key == request.key) return;
                it = pending.erase(it);
            } else ++it;
        }
        if (busy && active.owner == request.owner && active.key == request.key) return;
        // Rapid paging or weight changes cannot create an unbounded backlog.
        if (pending.size() == 32) pending.pop_front();
        pending.push_back(std::move(request));
        pump();
    }
private:
    QThreadPool pool;
    Request active;
    void pump() {
        if (busy) return;
        while (!pending.empty()) {
            auto request = std::move(pending.front());
            pending.pop_front();
            if (!request.owner || (!request.owner->isVisible() && !preparing(request.owner)) ||
                request.owner->property("font_preview_key").toString() != request.key) continue;
            if (cache.contains(request.key)) { request.owner->update(); continue; }
            busy = true;
            active = request;
            auto *task = new Task;
            task->work = [this, request] {
                QElapsedTimer timer;
                timer.start();
                QImage image;
                try {
                    if (identity(request.source.files) == request.source.identity) {
                        image = renderFontPreviewBitmap(request.source.files, request.source.family, request.text,
                            request.size, request.inkHeight, request.weight, request.boldReading, request.centered, request.wrap);
                        if (identity(request.source.files) != request.source.identity) image = {};
                    }
                } catch (const std::bad_alloc &) {
                    image = {};
                }
                const auto elapsed = timer.elapsed();
                qt::invoke(this, [this, request, image, elapsed] {
                    const int bytes = image.bytesPerLine() * image.height() + image.colorCount() * int(sizeof(QRgb)) +
                                      request.key.size() * 2 + 256;
                    cache.insert(request.key, new QImage(image), qMax(1, bytes));
                    setDiagnosticPreviewBytes(cache.totalCost());
                    diagnostic("font-preview-bitmap", {{"elapsed_ms", double(elapsed)},
                        {"bitmap_bytes", image.bytesPerLine() * image.height()}, {"cache_bytes", cache.totalCost()},
                        {"queued", int(pending.size())}, {"ok", !image.isNull()}}, false);
                    if (request.owner && request.owner->property("font_preview_key").toString() == request.key)
                        request.owner->update();
                    busy = false;
                    active = {};
                    pump();
                }, Qt::QueuedConnection);
            };
            pool.start(task);
            return;
        }
    }
};
Previews &previews() {
    static QPointer<Previews> instance;
    if (!instance) instance = new Previews;
    return *instance;
}
void finishPage(QWidget *page, int generation, const QStringList &keys) {
    if (page->property("font_preview_generation").toInt() != generation) return;
    for (const auto &key : keys) {
        if (!previews().cache.contains(key)) {
            qt::later(10, page, [page, generation, keys] { finishPage(page, generation, keys); });
            return;
        }
    }
    page->setProperty("font_preview_preparing", false);
    page->setProperty("font_preview_page_ready", true);
    page->setUpdatesEnabled(true);
    // Commit all children in this paint, including a page that replaced an
    // unfinished batch. Deferred child updates were discarded while disabled.
    page->repaint();
}

}
FontPreviewSource fontPreviewSource(const QString &family, const QStringList &files) {
    if (family.isEmpty() || files.isEmpty() || files.size() > 64) return {};
    return {family, files, identity(files)};
}
bool drawFontPreview(QWidget *owner, QPainter &painter, QRect area, const QString &text,
                     const FontPreviewSource &source, int inkHeight, int weight, bool boldReading, bool centered, bool wrap) {
    if (source.files.isEmpty() || source.identity.size() > 16384 || text.isEmpty() || text.size() > 256 ||
        area.width() <= 0 || area.height() <= 0 || area.width() > 2048 || area.height() > 512 ||
        qint64(area.width()) * area.height() > 256 * 1024) return false;
    const auto keyData = source.identity + source.family + '\n' + text + '\n' +
        QString::number(area.width()) + ':' + QString::number(area.height()) + ':' + QString::number(inkHeight) + ':' +
        QString::number(weight) + ':' + QString::number(boldReading) + ':' + QString::number(centered) + ':' + QString::number(wrap);
    const auto key = QString::fromLatin1(QCryptographicHash::hash(keyData.toUtf8(), QCryptographicHash::Sha256).toHex());
    auto &state = previews();
    owner->setProperty("font_preview_key", key);
    if (const auto *cached = state.cache.object(key)) {
        owner->setProperty("font_preview_ready", !cached->isNull());
        if (cached->isNull()) return false;
        auto colored = *cached;
        const auto ink = painter.pen().color();
        QVector<QRgb> colors(256);
        for (int i = 0; i < 256; ++i) colors[i] = qRgba(ink.red(), ink.green(), ink.blue(), i * ink.alpha() / 255);
        colored.setColorTable(colors);
        painter.drawImage(area.topLeft(), colored);
        return true;
    }
    state.request({owner, key, text, source, area.size(), inkHeight, weight, boldReading, centered, wrap});
    return false;
}
void prepareFontPreviewPage(QWidget *page) {
    // QWidget::render uses the controls' real paint geometry. A picture records
    // those commands without allocating a screen-sized image or loading fonts.
    if (page->property("font_preview_rendering").toBool()) return;
    page->setProperty("font_preview_rendering", true);
    page->setProperty("font_preview_preparing", true);
    page->setProperty("font_preview_page_ready", false);
    const int generation = page->property("font_preview_generation").toInt() + 1;
    page->setProperty("font_preview_generation", generation);
    page->setUpdatesEnabled(true);
    QStringList keys;
    page->installEventFilter(&previews());
    for (auto *label : page->findChildren<QWidget *>()) {
        label->installEventFilter(&previews());
        if (!label->property("font_preview_source").toBool() || !label->isVisibleTo(page)) continue;
        label->setProperty("font_preview_key", QString());
        QPicture commands;
        label->render(&commands);
        const auto key = label->property("font_preview_key").toString();
        if (!key.isEmpty()) keys.append(key);
    }
    page->setUpdatesEnabled(false);
    page->setProperty("font_preview_rendering", false);
    finishPage(page, generation, keys);
}
int fontPreviewCacheBytes() { return previews().cache.totalCost(); }
int fontPreviewPendingCount() { return int(previews().pending.size()) + (previews().busy ? 1 : 0); }
}
