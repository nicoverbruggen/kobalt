#include "engine_monitor.h"
#include "support/diagnostics.h"
#include "engine/engine_worker.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QHeaderView>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <deque>
#include <mutex>

namespace kobalt::desktop {
namespace {
constexpr int historyLimit = 600;
const QStringList lanes{ "Foreground",        "Background pages", "Chapter loads", "Page estimates",
                         "Previews / images", "Storage / other",  "Input to paint" };
const QColor colors[]{ QColor("#2868b2"), QColor("#16816d"), QColor("#9b61ac"), QColor("#ac7926"),
                       QColor("#4f8797"), QColor("#78838f"), QColor("#3d6d80") };

struct Span
{
    QString key, label;
    int lane = 0;
    double start = 0, end = -1;
    bool interrupted = false, failed = false, inferred = false;
};

struct Inbox
{
    std::mutex mutex;
    std::deque<QJsonObject> events;
    std::atomic_bool enabled{ false };
    std::atomic_int dropped{ 0 };

    void put(const QJsonObject &event)
    {
        if (!enabled.load())
            return;
        const auto name = event.value("event").toString();
        if (!(name.startsWith("render-") || name.startsWith("engine-work-")
              || name.startsWith("operation-") || name.startsWith("preview-")
              || name == "image-load-end" || name == "frame-ready" || name == "page-turn-painted"
              || name == "engine-cache-reuse" || name == "monitor-memory"))
            return;
        std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || events.size() >= 1024) {
            ++dropped;
            return;
        }
        events.push_back(event);
    }
};

QString duration(double ms)
{
    return QString::number(ms, 'f', ms < 10 ? 1 : 0) + " ms";
}

QString memory(const QJsonObject &fields, const char *key, double divisor = 1024)
{
    const auto value = fields.value(key);
    return value.isDouble() && value.toDouble() >= 0
            ? QString::number(value.toDouble() / divisor, 'f', 1) + " MiB"
            : QStringLiteral("Unavailable");
}

class Graph final : public QWidget
{
public:
    QVector<Span> spans;
    QVector<QPointF> rss;
    double now = 0, window = 10000;

    explicit Graph(QWidget *parent) : QWidget(parent) { setMinimumHeight(320); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), palette().base());
        const int left = qMax(160, fontMetrics().horizontalAdvance("Background pages") + 24),
                  right = width() - 18, top = 26, row = 29;
        const double first = now - window;
        const auto x = [&](double t) { return left + (t - first) / window * (right - left); };
        p.setPen(palette().text().color());
        for (int i = 0; i <= 6; ++i) {
            const auto pos = left + (right - left) * i / 6.;
            p.drawText(QRectF(pos - 30, 0, 60, 20), Qt::AlignCenter,
                       i == 6 ? "now" : QString("−%1 s").arg((6 - i) * window / 6000., 0, 'g', 3));
            p.setPen(palette().mid().color());
            p.drawLine(QPointF(pos, top), QPointF(pos, top + lanes.size() * row));
            p.setPen(palette().text().color());
        }
        for (int i = 0; i < lanes.size(); ++i) {
            p.drawText(QRect(8, top + i * row, left - 16, row), Qt::AlignVCenter, lanes[i]);
            p.setPen(palette().alternateBase().color());
            p.drawLine(left, top + (i + 1) * row, right, top + (i + 1) * row);
            p.setPen(palette().text().color());
        }
        p.save();
        p.setClipRect(QRect(left, top, right - left, lanes.size() * row));
        for (const auto &span : spans) {
            const double end = span.end < 0 ? now : span.end;
            if (end < first || span.start > now)
                continue;
            const QRectF bar(qMax(double(left), x(span.start)), top + span.lane * row + 6,
                             qMax(2., x(end) - qMax(double(left), x(span.start))), row - 12);
            const QColor color = span.failed ? QColor("#b83939")
                    : span.interrupted       ? QColor("#c47d23")
                                             : colors[span.lane];
            p.fillRect(bar, QBrush(color, span.end < 0 ? Qt::Dense4Pattern : Qt::SolidPattern));
            if (span.interrupted) {
                p.setPen(palette().base().color());
                p.drawLine(bar.topLeft(), bar.bottomRight());
            }
        }
        p.restore();
        const int bottom = top + lanes.size() * row + 22;
        double low = 1e30, high = 0;
        for (const auto &point : rss)
            if (point.x() >= first) {
                low = qMin(low, point.y());
                high = qMax(high, point.y());
            }
        p.setPen(palette().text().color());
        p.drawText(QRect(8, bottom, left - 16, 40), Qt::AlignVCenter,
                   high > 0 ? QString("Process memory\n%1–%2 MiB")
                                      .arg(low, 0, 'f', 0)
                                      .arg(high, 0, 'f', 0)
                            : "Process memory\nWaiting for sample");
        QPainterPath path;
        bool firstPoint = true;
        for (const auto &point : rss) {
            if (point.x() < first)
                continue;
            QPointF pos(x(point.x()), bottom + 38 - (point.y() - low) / qMax(1., high - low) * 34);
            if (firstPoint)
                path.moveTo(pos);
            else
                path.lineTo(pos);
            firstPoint = false;
        }
        p.setPen(QPen(colors[0], 2));
        p.drawPath(path);
        if (!firstPoint)
            p.drawEllipse(path.currentPosition(), 2, 2);
    }
};
} // namespace

struct EngineMonitor::State
{
    std::shared_ptr<Inbox> inbox = std::make_shared<Inbox>();
    QElapsedTimer clock;
    double eventTime = 0;
    Graph *graph;
    QLabel *summary, *resources, *status;
    QTableWidget *table;
    QComboBox *filter;
    QTimer timer;
    bool paused = false;
    int ticks = 0, finished = 0, interrupted = 0, failed = 0, reused = 0;
    int cachedPages = -1, loadedPages = -1;
    QJsonObject resourceFields;
    double fraction = -1, lastQueue = -1, lastPaint = -1, lastTotal = -1, lastEmulated = 0;
    QHash<QString, double> queued;
    QVector<double> renders;
    QJsonObject memoryFields;

    void addSpan(const QJsonObject &event)
    {
        const auto name = event.value("event").toString();
        const double time = event.value("monotonic_ms").toDouble();
        const double elapsed = event.value("elapsed_ms").toDouble();
        if (time > eventTime) {
            eventTime = time;
            clock.restart();
        }
        if (event.contains("cached_pages"))
            cachedPages = event.value("cached_pages").toInt();
        if (event.contains("loaded_pages"))
            loadedPages = event.value("loaded_pages").toInt();
        if (event.contains("resources"))
            resourceFields = event.value("resources").toObject();
        if (event.contains("book_fraction"))
            fraction = event.value("book_fraction").toDouble();
        if (event.contains("emulated_wait_ms"))
            lastEmulated = event.value("emulated_wait_ms").toDouble();
        if (name == "monitor-memory") {
            memoryFields = event;
            if (event.contains("rss_kib")) {
                graph->rss.append({ time, event.value("rss_kib").toDouble() / 1024. });
                if (graph->rss.size() > historyLimit)
                    graph->rss.removeFirst();
            }
            return;
        }
        if (name == "engine-cache-reuse") {
            ++reused;
            return;
        }
        const auto renderKey = QString::number(event.value("render_id").toDouble(), 'f', 0);
        if (name == "render-queued") {
            if (queued.size() >= historyLimit)
                queued.clear();
            queued[renderKey] = time;
            return;
        }
        if (name == "render-start" && queued.contains(renderKey))
            lastQueue = qMax(0., time - queued.take(renderKey));
        if (name == "frame-ready")
            return;
        if (name == "page-turn-painted") {
            lastTotal = elapsed;
            lastPaint = event.value("paint_ms").toDouble();
        }
        Span span;
        span.start = time;
        const bool start = name.endsWith("-start");
        if (name.startsWith("render-")) {
            span.key = "render:" + QString::number(event.value("render_id").toDouble(), 'f', 0);
            span.label = event.value("page_turn").toBool() ? "Page turn"
                    : event.value("jump").toBool()         ? "Jump"
                                                           : "Render / reflow";
        } else if (name.startsWith("engine-work-")) {
            span.label = event.value("work").toString();
            span.lane = span.label == "Page estimates" ? 3 : span.label == "Adjacent page preview" ? 4 : 1;
            span.key = "work:" + QString::number(event.value("work_id").toDouble(), 'f', 0);
        } else if (name.startsWith("operation-")) {
            span.label = event.value("operation").toString();
            span.lane = span.label == "chapter-load" || span.label == "chapter-turn"
                            || span.label == "engine-open"
                    ? 2
                    : 5;
            span.key =
                    "operation:" + QString::number(event.value("operation_id").toDouble(), 'f', 0);
        } else if (name == "page-turn-painted") {
            span.label = "Input to paint";
            span.lane = 6;
            span.key = "paint:" + renderKey;
        } else {
            span.label = name.startsWith("preview-") ? "Preview" : "Deferred image";
            span.lane = 4;
            span.key = span.label + event.value("thread").toString();
        }
        if (start) {
            graph->spans.append(span);
        } else {
            int found = -1;
            for (int i = graph->spans.size() - 1; i >= 0; --i)
                if (graph->spans[i].key == span.key && graph->spans[i].end < 0) {
                    found = i;
                    break;
                }
            if (found >= 0)
                span = graph->spans[found];
            else {
                span.start = time - elapsed;
                span.inferred = true;
            }
            span.end = time;
            span.interrupted = event.value("interrupted").toBool()
                    || (event.contains("completed") && !event.value("completed").toBool());
            span.failed = event.value("failed").toBool();
            if (found >= 0)
                graph->spans[found] = span;
            else
                graph->spans.append(span);
            ++finished;
            interrupted += span.interrupted;
            failed += span.failed;
            if (span.lane == 0) {
                renders.append(elapsed);
                if (renders.size() > historyLimit)
                    renders.removeFirst();
            }
            table->insertRow(0);
            const QStringList cells{ QString::number(time / 1000., 'f', 3), lanes[span.lane],
                                     span.label, duration(elapsed),
                                     span.failed                ? "Failed"
                                             : span.interrupted ? "Interrupted"
                                             : name == "operation-end" && !event.contains("completed") ? "Ended"
                                             : span.inferred    ? "Finished (start not observed)"
                                                                : "Finished" };
            for (int col = 0; col < cells.size(); ++col)
                table->setItem(0, col, new QTableWidgetItem(cells[col]));
            if (table->rowCount() > 150)
                table->removeRow(table->rowCount() - 1);
        }
        if (graph->spans.size() > historyLimit)
            graph->spans.removeFirst();
    }
};

EngineMonitor::EngineMonitor(QWidget *parent) : QWidget(parent, Qt::Window), state(new State)
{
    setObjectName("engineMonitor");
    setWindowTitle(tr("Kobalt · Engine monitor"));
    setAttribute(Qt::WA_QuitOnClose, false);
    // The developer toolbar uses standard Fusion widgets at desktop point sizes.
    auto desktopFont = font();
    desktopFont.setPointSize(11);
    setFont(desktopFont);
    resize(1000, 800);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(12);
    auto *controls = new QHBoxLayout;
    auto *title = new QLabel(tr("Engine activity"), this);
    auto titleFont = font();
    titleFont.setPointSize(17);
    titleFont.setBold(true);
    title->setFont(titleFont);
    controls->addWidget(title);
    controls->addStretch();
    auto *range = new QComboBox(this);
    for (const int seconds : { 2, 5, 10, 30, 60, 120 })
        range->addItem(tr("Last %1 seconds").arg(seconds), seconds);
    range->setCurrentIndex(2);
    range->setObjectName("engineMonitorRange");
    auto *zoomIn = new QPushButton(tr("+"), this);
    zoomIn->setObjectName("engineMonitorZoomIn");
    zoomIn->setToolTip(tr("Zoom in on the timeline"));
    zoomIn->setFixedWidth(30);
    auto *zoomOut = new QPushButton(tr("−"), this);
    zoomOut->setObjectName("engineMonitorZoomOut");
    zoomOut->setToolTip(tr("Zoom out on the timeline"));
    zoomOut->setFixedWidth(30);
    controls->addWidget(zoomOut);
    controls->addWidget(range);
    controls->addWidget(zoomIn);
    connect(zoomIn, &QPushButton::clicked, range,
            [range] { range->setCurrentIndex(qMax(0, range->currentIndex() - 1)); });
    connect(zoomOut, &QPushButton::clicked, range, [range] {
        range->setCurrentIndex(qMin(range->count() - 1, range->currentIndex() + 1));
    });
    auto *slow = new QPushButton(tr("Fake Kobo · 20×"), this);
    slow->setObjectName("engineMonitorSlowdown");
    slow->setCheckable(true);
    slow->setChecked(engineSlowdownEnabled());
    slow->setToolTip(tr(
            "Slow page rendering, background page preparation, chapter loads and chapter-boundary turns by 20× on the worker. Closing the monitor turns it off. This is not a hardware benchmark."));
    controls->addWidget(slow);
    connect(slow, &QPushButton::toggled, this, [this](bool enabled) {
        setEngineSlowdownEnabled(enabled);
        refresh();
    });
    auto *pause = new QPushButton(tr("Pause"), this);
    pause->setCheckable(true);
    pause->setObjectName("engineMonitorPause");
    controls->addWidget(pause);
    auto *clear = new QPushButton(tr("Clear"), this);
    clear->setObjectName("engineMonitorClear");
    controls->addWidget(clear);
    layout->addLayout(controls);
    state->summary = new QLabel(this);
    state->summary->setObjectName("engineMonitorSummary");
    state->summary->setWordWrap(true);
    layout->addWidget(state->summary);
    state->graph = new Graph(this);
    layout->addWidget(state->graph);
    state->resources = new QLabel(this);
    state->resources->setObjectName("engineMonitorResources");
    state->resources->setWordWrap(true);
    layout->addWidget(state->resources);
    auto *eventsHeader = new QHBoxLayout;
    eventsHeader->addWidget(new QLabel(tr("Recent completed work"), this));
    eventsHeader->addStretch();
    state->filter = new QComboBox(this);
    state->filter->setObjectName("engineMonitorFilter");
    state->filter->addItems({ tr("Engine work"), tr("All work"), tr("Storage / other") });
    eventsHeader->addWidget(state->filter);
    layout->addLayout(eventsHeader);
    connect(state->filter, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this] { refresh(); });
    state->table = new QTableWidget(0, 5, this);
    state->table->setObjectName("engineMonitorEvents");
    state->table->setHorizontalHeaderLabels(
            { "Clock (s)", "Work", "Operation", "Duration", "Result" });
    state->table->verticalHeader()->hide();
    state->table->verticalHeader()->setDefaultSectionSize(24);
    state->table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    state->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    state->table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    state->table->setSelectionBehavior(QAbstractItemView::SelectRows);
    state->table->setAlternatingRowColors(true);
    layout->addWidget(state->table, 1);
    state->status = new QLabel(this);
    state->status->setObjectName("engineMonitorStatus");
    state->status->setWordWrap(true);
    layout->addWidget(state->status);
    connect(range, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, range] {
        state->graph->window = range->currentData().toInt() * 1000.;
        state->graph->update();
    });
    connect(pause, &QPushButton::toggled, this, [this, pause](bool checked) {
        state->paused = checked;
        state->inbox->enabled = !checked && isVisible();
        pause->setText(checked ? tr("Resume") : tr("Pause"));
        refresh();
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        std::unique_lock<std::mutex> lock(state->inbox->mutex, std::try_to_lock);
        if (!lock.owns_lock())
            return;
        state->inbox->events.clear();
        state->inbox->dropped = 0;
        state->graph->spans.clear();
        state->graph->rss.clear();
        state->renders.clear();
        state->table->setRowCount(0);
        state->finished = state->interrupted = state->failed = state->reused = 0;
        state->queued.clear();
        state->lastQueue = state->lastPaint = state->lastTotal = -1;
        state->lastEmulated = 0;
        lock.unlock();
        refresh();
    });
    state->timer.setInterval(100);
    connect(&state->timer, &QTimer::timeout, this, [this] {
        refresh();
        if (!state->paused && ++state->ticks % 10 == 0) {
            const auto sink = observer();
            sampleDiagnosticMemory([sink](QJsonObject fields) {
                fields["event"] = QStringLiteral("monitor-memory");
                fields["monotonic_ms"] = fields.value("resource_sample_monotonic_ms");
                sink(fields);
            });
        }
    });
    state->clock.start();
    refresh();
}

EngineMonitor::~EngineMonitor()
{
    state->inbox->enabled = false;
}

std::function<void(const QJsonObject &)> EngineMonitor::observer() const
{
    const auto inbox = state->inbox;
    return [inbox](const QJsonObject &event) { inbox->put(event); };
}

void EngineMonitor::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    findChild<QPushButton *>("engineMonitorSlowdown")->setChecked(engineSlowdownEnabled());
    state->inbox->enabled = !state->paused;
    state->timer.start();
}

void EngineMonitor::hideEvent(QHideEvent *event)
{
    setEngineSlowdownEnabled(false);
    state->inbox->enabled = false;
    state->timer.stop();
    QWidget::hideEvent(event);
}

void EngineMonitor::refresh()
{
    if (!state->paused) {
        std::deque<QJsonObject> events;
        {
            std::unique_lock<std::mutex> lock(state->inbox->mutex, std::try_to_lock);
            if (lock.owns_lock())
                events.swap(state->inbox->events);
        }
        for (const auto &event : events)
            state->addSpan(event);
        state->graph->now = state->eventTime + state->clock.elapsed();
    }
    auto sorted = state->renders;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) {
        if (sorted.isEmpty())
            return QStringLiteral("—");
        const double index = p * (sorted.size() - 1);
        const int low = int(std::floor(index)), high = int(std::ceil(index));
        return duration(sorted[low] + (sorted[high] - sorted[low]) * (index - low));
    };
    state->summary->setText(
            tr("Renders %1   ·   Last %2   ·   Median %3   ·   P95 %4   ·   Worst %5\n"
               "Ended spans %6   ·   Interrupted %7   ·   Failed %8   ·   Chapter reuse %9\n"
               "Last queue wait %10   ·   Input to paint %11   ·   Paint %12   ·   Added delay %13")
                    .arg(state->renders.size())
                    .arg(state->renders.isEmpty() ? "—" : duration(state->renders.last()))
                    .arg(percentile(.5))
                    .arg(percentile(.95))
                    .arg(percentile(1))
                    .arg(state->finished)
                    .arg(state->interrupted)
                    .arg(state->failed)
                    .arg(state->reused)
                    .arg(state->lastQueue < 0 ? "—" : duration(state->lastQueue))
                    .arg(state->lastTotal < 0 ? "—" : duration(state->lastTotal))
                    .arg(state->lastPaint < 0 ? "—" : duration(state->lastPaint))
                    .arg(duration(state->lastEmulated)));
    state->resources->setText(
            tr("Process RSS %1   ·   Peak %2   ·   Above opening baseline %3\n"
               "Font memory %4   ·   Cached pages %5 / %7 loaded   ·   Book position %6\n"
               "Resource cache %8 / %9   ·   Image queue %10   ·   Evictions %11")
                    .arg(memory(state->memoryFields, "rss_kib"))
                    .arg(memory(state->memoryFields, "peak_rss_kib"))
                    .arg(memory(state->memoryFields, "kobalt_rss_kib"))
                    .arg(memory(state->memoryFields, "private_font_bytes", 1048576))
                    .arg(state->cachedPages < 0 ? "—" : QString::number(state->cachedPages))
                    .arg(state->fraction < 0 ? "Whole-book mode / unavailable"
                                             : QString::number(state->fraction * 100, 'f', 1) + "%")
                    .arg(state->loadedPages < 0 ? "—" : QString::number(state->loadedPages))
                    .arg(memory(state->resourceFields, "allocated_bytes", 1048576))
                    .arg(memory(state->resourceFields, "budget_bytes", 1048576))
                    .arg(state->resourceFields.contains("pending_images")
                                 ? QString::number(
                                           state->resourceFields.value("pending_images").toInt())
                                 : "—")
                    .arg(state->resourceFields.contains("evictions")
                                 ? QString::number(state->resourceFields.value("evictions").toInt())
                                 : "—"));
    state->status->setText(
            tr("%1 · %2 events dropped · %3\n"
               "Striped bars are unfinished; amber means interrupted. "
               "Recent 600 spans / renders; memory sampled each second. Timings include diagnostic overhead, not physical e-ink refresh.")
                    .arg(state->paused ? "Paused; new events are not recorded" : "Live")
                    .arg(state->inbox->dropped.load())
                    .arg(engineSlowdownEnabled() ? "FAKE KOBO: rendering slowed 20×"
                                                 : "Normal speed"));
    for (int row = 0; row < state->table->rowCount(); ++row) {
        const bool storage = state->table->item(row, 1)->text() == lanes[5];
        state->table->setRowHidden(row,
                                   state->filter->currentIndex() == 0           ? storage
                                           : state->filter->currentIndex() == 2 ? !storage
                                                                                : false);
    }
    state->graph->update();
}
} // namespace kobalt::desktop
