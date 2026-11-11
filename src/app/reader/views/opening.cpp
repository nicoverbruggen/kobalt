#include "support/diagnostics.h"
#include "ui/controls/buttons.h"
#include "ui/controls/icons.h"
#include "ui/controls/text.h"
#include "ui/controls/panels.h"
#include "ui/dialogs/modals.h"
#include "ui/style/theme.h"
#include "support/strings.h"
#include "opening.h"
#include "ui/style/fonts.h"
#include "ui/style/styles.h"
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QLocale>
#include <QTimer>
#include "ui/style/metrics.h"
#include "platform/api/kobo_refresh.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QPointer>
#include <QProgressBar>
#include <QPainter>
#include <QFontMetricsF>
#include <QScreen>
#include <QImageReader>
#include <QFile>
#include <QRunnable>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include "reading/reading_density.h"

namespace kobalt {
namespace {
struct CoverRead
{
    std::atomic_bool cancelled{ false };
    // Written by the job, then read on the GUI thread in `finished`.
    QImage image;
};

class CoverJob final : public QRunnable
{
public:
    QString path;
    std::shared_ptr<CoverRead> result;
    // Runs on the GUI thread after the read, unless it was cancelled first.
    std::function<void()> finished;

    void run() override
    {
        if (result->cancelled.load())
            return;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 16 * 1024 * 1024) {
            QImageReader reader(&file);
            const auto size = reader.size();
            if (size.isValid() && qint64(size.width()) * size.height() <= 64 * 1024 * 1024) {
                if (size.width() > 2048 || size.height() > 2048)
                    reader.setScaledSize(size.scaled(QSize(2048, 2048), Qt::KeepAspectRatio));
                if (!result->cancelled.load())
                    result->image = reader.read();
            }
        }
        kobalt::qt::invoke(QCoreApplication::instance(), finished, Qt::QueuedConnection);
    }
};

// An opaque child keeps cover damage and its refresh policy out of the loading controls.
class OpeningCover final : public QWidget
{
public:
    explicit OpeningCover(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrOpeningCover");
        setAttribute(Qt::WA_OpaquePaintEvent);
        hide();
    }

    QImage image;

    void setImage(QImage next)
    {
        if (image == next)
            return;
        image = std::move(next);
        renderImage();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        renderImage();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.drawImage(QPoint(), pixels);
    }

private:
    QImage pixels;
    QRect imageBounds;

    void renderImage()
    {
        if (image.isNull() || size().isEmpty())
            return;
        const auto ratio = devicePixelRatio();
        QImage next(size() * ratio, QImage::Format_RGB32);
        next.setDevicePixelRatio(ratio);
        next.fill(palette().color(QPalette::Window));
        const auto fitted = QSizeF(image.size()).scaled(QSizeF(size()), Qt::KeepAspectRatio);
        const QRectF bounds((width() - fitted.width()) / 2, (height() - fitted.height()) / 2,
                            fitted.width(), fitted.height());
        QPainter painter(&next);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(bounds, image);
        painter.end();
        if (pixels == next)
            return;
        const auto damage = imageBounds.united(bounds.toAlignedRect());
        pixels = std::move(next);
        imageBounds = bounds.toAlignedRect();
        update(damage);
    }
};

class OpeningRedirect final : public QWidget
{
public:
    explicit OpeningRedirect(QWidget *parent) : QWidget(parent)
    {
        setObjectName("rrOpeningRedirect");
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        ui::paintOpeningIcon(painter, ui::ControlIcon::Redirect, rect(),
                             palette().color(QPalette::WindowText));
    }
};

class OpeningDialog final : public QDialog
{
public:
    OpeningDialog(QWidget *background, bool dark, PresentationFactory makePresentation,
                  bool embedded = false)
        : QDialog(background, embedded ? Qt::SubWindow : Qt::Dialog | Qt::FramelessWindowHint),
          presentation(makePresentation(this)),
          embedded(embedded)
    {
        Q_ASSERT(presentation);
        // Start as a child. Creating a dialog under an embedded reader first
        // leaves a separate graphics proxy even after clearing its window flag.
        setObjectName("rrOpening");
        setWindowModality(embedded ? Qt::NonModal : Qt::ApplicationModal);
        setProperty("reader_dark", dark);
        setAutoFillBackground(true);
        QPalette colors = ui::readerPalette(palette(), dark);
        colors.setColor(QPalette::Midlight, ui::themeColors(dark).openingTrack);
        setPalette(colors);
        setFont(ui::interfaceFont());
        // The card stylesheet otherwise uses the application font on Qt 6.
        ui::readerStyle(this, ui::styles::reader());
        cover = new OpeningCover(this);
        brand = new ui::InterfaceLabel(strings::get("app.wordmark"), this);
        brand->setObjectName("rrOpeningBrand");
        brand->setAlignment(Qt::AlignCenter);
        label = new ui::InterfaceLabel(strings::get("opening.loading"), this);
        label->setObjectName("rrOpeningLabel");
        label->setAlignment(Qt::AlignCenter);
        redirect = new OpeningRedirect(this);
        progress = new QProgressBar(this);
        progress->setObjectName("rrOpeningProgress");
        progress->setRange(0, 100);
        progress->setValue(0);
        progress->setTextVisible(false);
        ui::readerStyle(
                progress,
                "QProgressBar{border:0;padding:0;margin:0;min-height:0;background:palette(midlight);} QProgressBar::chunk{background:palette(window-text);}");
        // Font capitalization does not reach this dialog's labels, so set the
        // caption's capitals in the text itself.
        hint = new ui::InterfaceLabel(QLocale().toUpper(strings::get("opening.may_take_a_while")),
                                      this);
        hint->setObjectName("rrOpeningHint");
        hint->setAlignment(Qt::AlignCenter);
        hint->setText(QLocale().toUpper(strings::get("opening.preparing")));
        hintTimer.setSingleShot(true);
        hintTimer.setTimerType(Qt::PreciseTimer);
        connect(&hintTimer, &QTimer::timeout, this, [this] {
            if (isVisible()) {
                delayed = true;
                hint->setText(QLocale().toUpper(strings::get("opening.may_take_a_while")));
            }
        });
        cancel = new ui::BackButton(QString(), this);
        cancel->setObjectName("rrCancelOpening");
        cancel->setAccessibleName(strings::get("common.cancel"));
        cancel->setVisible(!embedded);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        for (auto *widget : findChildren<QWidget *>())
            platform::textRefresh(widget);
        platform::textRefresh(this);
        platform::coverRefresh(cover);
    }

    void setCover(QImage image)
    {
        // An early cache miss is not proof that the publication has no cover.
        if (image.isNull())
            return;
        // Compare the source pixels per painted pixel after fitting the cover.
        // A late thumbnail must not replace a sharper image delivered earlier.
        const auto density = [](const QImage &candidate) {
            const auto area = ui::metrics::opening::cover.size();
            return qMax(candidate.width() / area.width(), candidate.height() / area.height());
        };
        if (!cover->image.isNull() && density(image) < density(cover->image))
            return;
        const bool logo = !brand->isHidden();
        cover->setImage(std::move(image));
        cover->show();
        showBrand();
        // The cover's drawn shape decides where the whole group sits. Lay out
        // again only when that moves something, so an identical or sharper
        // copy of the cover damages nothing outside the cover itself.
        const auto nextLift = centringLift();
        if (logo != !brand->isHidden() || nextLift != lift
            || placed(coverRect(), nextLift) != cover->geometry()) {
            layoutWidgets();
            update();
        }
    }

    ~OpeningDialog() override
    {
        if (coverRead)
            coverRead->cancelled.store(true);
    }

    void loadCover(const QString &path)
    {
        if (path.isEmpty())
            return;
        if (coverRead)
            coverRead->cancelled.store(true);
        coverRead = std::make_shared<CoverRead>();
        auto *job = new CoverJob;
        job->path = path;
        job->result = coverRead;
        // The job reports its end; nothing polls for it. A newer read or a
        // closed dialog drops the report.
        const QPointer<OpeningDialog> guard(this);
        job->finished = [guard, read = coverRead] {
            if (!guard || guard->coverRead != read)
                return;
            auto image = std::move(read->image);
            guard->coverRead.reset();
            guard->setCover(std::move(image));
        };
        QThreadPool::globalInstance()->start(job);
    }

    void setProgress(int percent)
    {
        auto *bar = progress;
        const int value = qMax(bar->value(), percent);
        if (bar->value() == value)
            return;
        // QProgressBar can repaint synchronously. Let this event-loop pass
        // finish so later progress or closing can replace obsolete values.
        const bool updates = bar->updatesEnabled();
        if (updates)
            bar->setUpdatesEnabled(false);
        bar->setValue(value);
        if (updates)
            bar->setUpdatesEnabled(true);
    }

    void showFallback(OpeningFallback state)
    {
        fallback = true;
        automatic = state == OpeningFallback::Protected || state == OpeningFallback::Remembered;
        storageFailure = state == OpeningFallback::Storage;
        const bool protectedBook = state == OpeningFallback::Protected;
        hintTimer.stop();
        progress->setVisible(automatic);
        if (automatic)
            setProgress(progress->maximum());
        redirect->setVisible(automatic);
        cancel->hide();
        label->setText(automatic ? strings::get("opening.native") : strings::get("opening.failed"));
        hint->setText(QLocale().toUpper(
                strings::get(protectedBook ? "opening.protected" : "opening.remembered")));
        hint->setVisible(automatic);
        showBrand();
        label->setVisible(automatic);
        if (automatic) {
            if (failureDialog)
                failureDialog->hide();
        } else {
            auto *dialog = failure();
            dialog->show();
            dialog->fitCard();
            dialog->raise();
            dialog->setFocus();
        }
        layoutWidgets();
        if (!automatic)
            update();
    }

    void afterFirstPaint(std::function<void()> callback)
    {
        if (painted) {
            scheduleAfterPaint(std::move(callback));
        } else {
            firstPaint = std::move(callback);
        }
    }

    void setReopening()
    {
        if (!fallback && !delayed)
            hint->setText(QLocale().toUpper(strings::get("opening.reopening")));
    }

    void refreshFonts()
    {
        setFont(ui::interfaceFont());
        ui::readerStyle(this, ui::styles::reader());
        layoutWidgets();
        ui::refreshInterfaceText(this);
    }

    void reject() override
    {
        if (embedded)
            emit rejected();
        else
            QDialog::reject();
    }

    void accept() override
    {
        if (!fallback || !parentWidget()) {
            QDialog::accept();
            return;
        }
        presentation->acceptHandoff(*this, [this] { QDialog::accept(); });
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        QDialog::showEvent(event);
        if (!fallback) {
            hint->show();
            hintTimer.start(ui::metrics::opening::hintDelayMilliseconds);
        }
    }

    void hideEvent(QHideEvent *event) override
    {
        hintTimer.stop();
        QDialog::hideEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QDialog::paintEvent(event);
        namespace opening = ui::metrics::opening;
        QPainter painter(this);
        // Nickel's Qt5 stylesheet paints dialogs white even with a dark palette.
        // Own the whole splash background, including the area behind the cover.
        painter.fillRect(rect(), palette().color(QPalette::Window));
        if (!painted) {
            painted = true;
            if (property("kobalt_tile_cover_probe").toBool()) {
                setProperty("kobalt_first_cover_key", QString::number(cover->image.cacheKey()));
                diagnostic(
                        "tile-cover-probe-first-paint",
                        qt::object({ { "width", cover->image.width() },
                                     { "height", cover->image.height() },
                                     { "image_key", QString::number(cover->image.cacheKey()) } }));
            }
            if (firstPaint) {
                scheduleAfterPaint(std::move(firstPaint));
            }
        }
        painter.translate(origin() + QPointF(0, lift));
        painter.scale(scale(), scale());
        const auto ink = palette().color(QPalette::WindowText);
        if (cover->image.isNull() && (!fallback || automatic)) {
            ui::paintBrandIcon(painter, opening::icon, ink);
        }
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QDialog::resizeEvent(event);
        layoutWidgets();
    }

    void layoutWidgets()
    {
        namespace opening = ui::metrics::opening;
        const auto place = [this](QWidget *widget, const QRectF &rect) {
            widget->setGeometry(placed(rect, lift));
        };
        const auto textStyle = [this](QLabel *widget, qreal pixels, bool italic, int minimum = 1) {
            const auto font = ui::brandFont(qMax(minimum, qRound(pixels * scale())), italic);
            widget->setFont(font);
            ui::readerStyle(
                    widget,
                    QString("background:palette(window);color:palette(window-text);font-family:'%1';font-size:%2px;font-weight:400;font-style:%3;")
                            .arg(font.family())
                            .arg(font.pixelSize())
                            .arg(italic ? "italic" : "normal"));
        };
        textStyle(brand, opening::brandText, false);
        const bool phone = isPhoneScreen(size(), ui::density());
        textStyle(label, !fallback || automatic ? opening::labelText : opening::failureText,
                  !fallback || automatic, phone && (!fallback || automatic) ? int(ui::px(32)) : 1);
        // A caption in capitals, like the reader's other captions.
        ui::captionLabel(hint,
                         qMax(phone ? int(ui::px(22)) : 1, qRound(opening::hintText * scale())));
        lift = centringLift();
        place(cover, coverRect());
        place(hint, opening::hint);
        progress->setFixedHeight(qMax(1, qRound(opening::progress.height() * scale())));
        place(brand, fallback && !automatic ? opening::failureBrand : opening::brand);
        place(label, fallback && !automatic ? opening::failureLabel : opening::label);
        place(progress, opening::progress);
        if (phone) {
            for (auto *text : { label, hint }) {
                auto bounds = text->geometry();
                const int height = qMax(bounds.height(), ui::interfaceLineHeight(text->font()));
                bounds.setTop(bounds.center().y() - height / 2);
                bounds.setHeight(height);
                text->setGeometry(bounds);
            }
        }

        if (automatic) {
            const int gap = qMax(1, qRound(opening::handoffGap * scale()));
            int iconSize = label->font().pixelSize();
            const int padding = 2 * qMax(1, qRound(2 * scale()));
            int textWidth = ui::interfaceTextWidth(label->text(), label->font()) + padding;
            const int available = width() - 2 * qMax(1, qRound(16 * scale()));
            if (textWidth + gap + iconSize > available) {
                const int pixels =
                        qMax(1, int(iconSize * qreal(available - gap) / (textWidth + iconSize)));
                textStyle(label, pixels / scale(), true);
                iconSize = label->font().pixelSize();
                textWidth = ui::interfaceTextWidth(label->text(), label->font()) + padding;
            }
            while (ui::interfaceTextWidth(hint->text(), hint->font()) > hint->width()
                   && hint->font().pixelSize() > 1)
                ui::captionLabel(hint, hint->font().pixelSize() - 1);
            auto row = label->geometry();
            row.setWidth(iconSize + gap + textWidth);
            row.moveCenter(QPoint(width() / 2, row.center().y()));
            redirect->setGeometry(row.left(),
                                  row.center().y() - iconSize / 2
                                          + qRound(opening::handoffIconDrop * scale()),
                                  iconSize, iconSize);
            label->setGeometry(row.left() + iconSize + gap, row.top(), textWidth, row.height());
        }
        namespace navigation = ui::metrics::navigation;
        cancel->setGeometry(navigation::backLeft, navigation::headingTop, navigation::height,
                            navigation::height);
        ui::readerStyle(
                cancel,
                "QPushButton{min-height:0;border:none;padding:0;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}");
    }

private:
    void showBrand() { brand->setVisible(cover->image.isNull() && (!fallback || automatic)); }

    // Most openings never fail. Laying out this card's text before the first
    // paint delayed every loading screen, so it is built on first use.
    ui::ChoiceModal *failure()
    {
        if (failureDialog)
            return failureDialog;
        failureDialog =
                new ui::ChoiceModal(this,
                                    { storageFailure ? strings::get("opening.storage_failed_detail")
                                                     : strings::get("opening.failed_detail"),
                                      storageFailure ? strings::get("opening.storage_failed")
                                                     : strings::get("opening.failed"),
                                      {} },
                                    { strings::get("opening.go_back"), "rrOpeningBack" },
                                    { strings::get("opening.try_native"), "rrOpeningNative" });
        failureDialog->setObjectName("rrOpeningFailure");
        connect(failureDialog->primaryButton(), &QPushButton::clicked, this, &QDialog::accept);
        connect(failureDialog->secondaryButton(), &QPushButton::clicked, this, &QDialog::reject);
        for (auto *widget : failureDialog->findChildren<QWidget *>())
            platform::textRefresh(widget);
        platform::textRefresh(failureDialog);
        return failureDialog;
    }

    void scheduleAfterPaint(std::function<void()> callback)
    {
        kobalt::qt::later(0, this, [this, callback = std::move(callback)] {
            if (isVisible()) {
                callback();
            }
        });
    }

    // A rectangle in reference units, in pixels on this screen.
    QRect placed(const QRectF &rect, qreal shift) const
    {
        const auto offset = origin() + QPointF(0, shift);
        const auto factor = scale();
        return { qRound(offset.x() + rect.x() * factor), qRound(offset.y() + rect.y() * factor),
                 qRound(rect.width() * factor), qRound(rect.height() * factor) };
    }

    // The cover as drawn: its own shape fitted to the cover area, resting on
    // the area's bottom edge so the text below keeps its distance from it.
    QRectF coverRect() const
    {
        const auto area = ui::metrics::opening::cover;
        if (cover->image.isNull())
            return area;
        const auto drawn = QSizeF(cover->image.size()).scaled(area.size(), Qt::KeepAspectRatio);
        return { area.left(), area.bottom() - drawn.height(), area.width(), drawn.height() };
    }

    // The vertical shift, in pixels, that centres the visible group on the
    // screen: from the top of the drawn cover, or of the logo, down to the
    // baseline of the hint's capitals. The failure card has its own layout.
    qreal centringLift() const
    {
        namespace opening = ui::metrics::opening;
        if (fallback && !automatic)
            return 0;
        const qreal top = cover->image.isNull() ? opening::icon.top() : coverRect().top();
        const QFontMetricsF metrics(hint->font());
        const qreal hintBaseline =
                opening::hint.center().y() * scale() + (metrics.ascent() - metrics.descent()) / 2;
        return (height() - (2 * origin().y() + top * scale() + hintBaseline)) / 2;
    }

    qreal scale() const
    {
        return qMin(width() / ui::metrics::opening::referenceWidth,
                    height() / ui::metrics::opening::referenceHeight);
    }

    QPointF origin() const
    {
        return { (width() - ui::metrics::opening::referenceWidth * scale()) / 2,
                 (height() - ui::metrics::opening::referenceHeight * scale()) / 2 };
    }

    OpeningCover *cover;
    qreal lift = 0;
    bool delayed = false;
    std::shared_ptr<CoverRead> coverRead;
    QProgressBar *progress;
    QLabel *brand, *label, *hint;
    OpeningRedirect *redirect;
    ui::ChoiceModal *failureDialog = nullptr;
    bool storageFailure = false;
    bool fallback = false, automatic = false;
    std::unique_ptr<WindowPresentation> presentation;
    const bool embedded;
    QTimer hintTimer;
    ui::Button *cancel;
    bool painted = false;
    std::function<void()> firstPaint;
};

class OpeningView final : public ReaderOpeningView
{
public:
    OpeningView(QWidget *background, const QJsonObject &preferences,
                PresentationFactory presentation, bool embedded = false)
        : dialog(new OpeningDialog(background, preferences.value("dark_mode").toBool(false),
                                   std::move(presentation), embedded))
    {
        if (embedded)
            return;
        auto *screen = QGuiApplication::primaryScreen();
        dialog->setGeometry(screen->geometry());
        QObject::connect(screen, &QScreen::geometryChanged, dialog,
                         [window = dialog](const QRect &geometry) {
                             if (window)
                                 window->setGeometry(geometry);
                         });
    }

    ~OpeningView() override
    {
        if (dialog) {
            dialog->hide();
            dialog->deleteLater();
        }
    }

    QDialog *window() const override { return dialog; }

    void showFallback(OpeningFallback state) override
    {
        if (dialog)
            dialog->showFallback(state);
    }

    void setCover(QImage cover) override
    {
        if (dialog)
            dialog->setCover(std::move(cover));
    }

    void loadCover(const QString &path) override
    {
        if (dialog)
            dialog->loadCover(path);
    }

    void setProgress(int percent) override
    {
        if (dialog)
            dialog->setProgress(percent);
    }

    void afterFirstPaint(std::function<void()> callback) override
    {
        if (dialog) {
            dialog->afterFirstPaint(std::move(callback));
        }
    }

    void setReopening() override
    {
        if (dialog)
            dialog->setReopening();
    }

    void refreshFonts() override
    {
        if (dialog) {
            dialog->refreshFonts();
        }
    }

private:
    QPointer<OpeningDialog> dialog;
};
} // namespace

std::unique_ptr<ReaderOpeningView>
createOpening(QWidget *background, const QJsonObject &preferences, PresentationFactory presentation)
{
    return std::make_unique<OpeningView>(background, preferences, std::move(presentation));
}

std::unique_ptr<ReaderOpeningView> createEmbeddedOpening(QWidget *parent,
                                                         PresentationFactory presentation)
{
    QJsonObject preferences;
    preferences["dark_mode"] = ui::darkTheme(parent);
    return std::make_unique<OpeningView>(parent, preferences, std::move(presentation), true);
}

SaveErrorView::SaveErrorView(QWidget *parent, const QString &error, bool retried)
    : ui::ChoiceModal(
              parent,
              { strings::get("closing.failed_detail")
                        + (error.isEmpty() ? QString() : "\n\n" + error)
                        + (retried ? "\n\n" + strings::get("closing.quit_warning") : QString()),
                strings::get("closing.failed"),
                {} },
              { strings::get("closing.return"), "rrCloseSaveReturn" },
              { strings::get("closing.retry"), "rrCloseSaveRetry" })
{
    setObjectName("rrCloseSaveError");
    if (retried) {
        quit = ui::makeModalButton(card(), { strings::get("closing.quit"), "rrCloseSaveQuit" },
                                   false);
        quit->ensurePolished();
        quit->setFixedHeight(ui::metrics::chrome::actionHeight);
        card()->rows()->addWidget(quit);
    }
    fitCard();
}

} // namespace kobalt
