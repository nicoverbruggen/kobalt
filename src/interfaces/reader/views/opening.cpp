#include "interfaces/reader/ui/controls/buttons.h"
#include "interfaces/reader/ui/controls/icons.h"
#include "interfaces/reader/ui/controls/text.h"
#include "interfaces/reader/ui/controls/panels.h"
#include "interfaces/reader/ui/dialogs/modals.h"
#include "interfaces/reader/ui/style/theme.h"
#include "support/strings.h"
#include "opening.h"
#include "interfaces/reader/ui/style/fonts.h"
#include "interfaces/reader/ui/style/styles.h"
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QLocale>
#include <QTimer>
#include "interfaces/reader/ui/style/metrics.h"
#include "platform/api/kobo_refresh.h"
#include "support/qt_compat.h"
#include <QApplication>
#include <QPointer>
#include <QProgressBar>
#include <QPainter>
#include <QScreen>
#include "reading/reading_density.h"

namespace kobalt {
namespace {
class OpeningDialog final : public QDialog {
public:
    OpeningDialog(QWidget *background, bool dark, PresentationFactory makePresentation, bool embedded = false)
        : QDialog(background, embedded ? Qt::SubWindow : Qt::Dialog | Qt::FramelessWindowHint),
          presentation(makePresentation(this)), embedded(embedded) {
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
        brand = new ui::InterfaceLabel(strings::get("app.wordmark"), this);
        brand->setObjectName("rrOpeningBrand");
        brand->setAlignment(Qt::AlignCenter);
        label = new ui::InterfaceLabel(strings::get("opening.loading"), this);
        label->setObjectName("rrOpeningLabel");
        label->setAlignment(Qt::AlignCenter);
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
        hint = new ui::InterfaceLabel(QLocale().toUpper(strings::get("opening.may_take_a_while")), this);
        hint->setObjectName("rrOpeningHint");
        hint->setAlignment(Qt::AlignCenter);
        hint->hide();
        hintTimer.setSingleShot(true);
        hintTimer.setTimerType(Qt::PreciseTimer);
        connect(&hintTimer, &QTimer::timeout, this, [this] {
            if (isVisible())
                hint->show();
        });
        cancel = new ui::BackButton(QString(), this);
        cancel->setObjectName("rrCancelOpening");
        cancel->setAccessibleName(strings::get("common.cancel"));
        cancel->setVisible(!embedded);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        detail = new ui::InterfaceLabel(this);
        detail->setObjectName("rrOpeningDetail");
        detail->setAlignment(Qt::AlignCenter);
        detail->setWordWrap(true);
        detail->hide();
        for (auto *widget : findChildren<QWidget *>())
            platform::textRefresh(widget);
        platform::textRefresh(this);
    }
    void setCover(QImage image) {
        if (fallback)
            return;
        cover = std::move(image);
        brand->setVisible(cover.isNull());
        update();
    }
    void showFallback(OpeningFallback state) {
        fallback = true;
        automatic = state == OpeningFallback::Protected || state == OpeningFallback::Remembered;
        storageFailure = state == OpeningFallback::Storage;
        const bool protectedBook = state == OpeningFallback::Protected;
        protectedIcon = protectedBook;
        hintTimer.stop();
        progress->hide();
        cancel->hide();
        label->setText(automatic ? strings::get("opening.native") : strings::get("opening.failed"));
        detail->setText(protectedBook ? strings::get("opening.protected")
                        : automatic   ? strings::get("opening.remembered")
                                      : strings::get("opening.failed_detail"));
        detail->show();
        hint->setText(QLocale().toUpper(strings::get("opening.continuing")));
        hint->setVisible(automatic);
        brand->setVisible(automatic);
        label->setVisible(automatic);
        detail->setVisible(automatic);
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
        update();
    }
    void afterFirstPaint(std::function<void()> callback) {
        if (painted) {
            scheduleAfterPaint(std::move(callback));
        } else {
            firstPaint = std::move(callback);
        }
    }
    void refreshFonts() {
        setFont(ui::interfaceFont());
        ui::readerStyle(this, ui::styles::reader());
        layoutWidgets();
        ui::refreshInterfaceText(this);
    }
    void reject() override {
        if (embedded)
            emit rejected();
        else
            QDialog::reject();
    }
    void accept() override {
        if (!fallback || !parentWidget()) {
            QDialog::accept();
            return;
        }
        presentation->acceptHandoff(*this, [this] {
            QDialog::accept();
        });
    }
    QProgressBar *progress;

protected:
    void showEvent(QShowEvent *event) override {
        QDialog::showEvent(event);
        if (!fallback) {
            hint->hide();
            hintTimer.start(ui::metrics::opening::hintDelayMilliseconds);
        }
    }
    void hideEvent(QHideEvent *event) override {
        hintTimer.stop();
        hint->hide();
        QDialog::hideEvent(event);
    }
    void paintEvent(QPaintEvent *event) override {
        QDialog::paintEvent(event);
        namespace m = ui::metrics::opening;
        QPainter painter(this);
        // Nickel's Qt5 stylesheet paints dialogs white even with a dark palette.
        // Own the whole splash background, including the area behind the cover.
        painter.fillRect(rect(), palette().color(QPalette::Window));
        if (!painted) {
            painted = true;
            if (firstPaint) {
                scheduleAfterPaint(std::move(firstPaint));
            }
        }
        if (fallback && !automatic)
            return;
        painter.translate(origin());
        painter.scale(scale(), scale());
        const auto ink = palette().color(QPalette::WindowText);
        if (!fallback && !cover.isNull()) {
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            const auto size = QSizeF(cover.size()).scaled(m::cover.size(), Qt::KeepAspectRatio);
            const QRectF bounds(m::cover.center() - QPointF(size.width() / 2, size.height() / 2), size);
            painter.drawImage(bounds, cover);
        } else {
            ui::paintBrandIcon(painter, fallback && !automatic ? m::failureBrandIcon : m::icon, ink);
        }
        if (fallback) {
            const QRectF bounds = automatic ? m::handoffIcon : m::failureIcon;
            ui::paintOpeningIcon(painter, protectedIcon ? ui::ControlIcon::Lock : ui::ControlIcon::FileWarning, bounds,
                                 ink);
        }
    }
    void resizeEvent(QResizeEvent *event) override {
        QDialog::resizeEvent(event);
        layoutWidgets();
    }
    void layoutWidgets() {
        namespace m = ui::metrics::opening;
        const auto place = [this](QWidget *widget, const QRectF &rect) {
            const auto offset = origin();
            const auto factor = scale();
            widget->setGeometry(qRound(offset.x() + rect.x() * factor), qRound(offset.y() + rect.y() * factor),
                                qRound(rect.width() * factor), qRound(rect.height() * factor));
        };
        const auto textStyle = [this](QLabel *widget, qreal pixels, bool italic, int minimum = 1) {
            const auto font = ui::brandFont(qMax(minimum, qRound(pixels * scale())), italic);
            widget->setFont(font);
            ui::readerStyle(
                widget,
                QString(
                    "background:palette(window);color:palette(window-text);font-family:'%1';font-size:%2px;font-weight:400;font-style:%3;")
                    .arg(font.family())
                    .arg(font.pixelSize())
                    .arg(italic ? "italic" : "normal"));
        };
        textStyle(brand, m::brandText, false);
        const bool phone = isPhoneScreen(size(), ui::density());
        textStyle(label,
                  !fallback   ? m::labelText
                  : automatic ? m::handoffText
                              : m::failureText,
                  !fallback || automatic, phone && !fallback ? int(ui::px(32)) : 1);
        // A caption in capitals, like the reader's other captions.
        ui::captionLabel(hint, qMax(phone ? int(ui::px(22)) : 1, qRound(m::hintText * scale())));
        place(hint, m::hint);
        progress->setFixedHeight(qMax(1, qRound(m::progress.height() * scale())));
        place(brand, fallback && !automatic ? m::failureBrand : m::brand);
        place(label, !fallback ? m::label : automatic ? m::handoffLabel : m::failureLabel);
        place(progress, m::progress);
        if (phone) {
            for (auto *text : {label, hint}) {
                auto bounds = text->geometry();
                const int height = qMax(bounds.height(), ui::interfaceLineHeight(text->font()));
                bounds.setTop(bounds.center().y() - height / 2);
                bounds.setHeight(height);
                text->setGeometry(bounds);
            }
        }

        if (fallback) {
            const int pixels = qMax(1, qRound(m::detailText * scale()));
            detail->setFont(ui::interfaceFont(pixels));
            ui::readerStyle(
                detail,
                QString("background:palette(window);color:palette(window-text);font-size:%1px;font-family:'%2';")
                    .arg(pixels)
                    .arg(ui::interfaceFontFamily()));
            place(detail, automatic ? m::handoffDetail : m::failureDetail);
            if (automatic)
                place(hint, m::handoffHint);
        }
        namespace n = ui::metrics::navigation;
        cancel->setGeometry(n::backLeft, n::headingTop, n::height, n::height);
        ui::readerStyle(
            cancel,
            "QPushButton{min-height:0;border:none;padding:0;background:palette(window);color:palette(window-text);} QPushButton:pressed{background:palette(midlight);}");
    }

private:
    // Most openings never fail. Laying out this card's text before the first
    // paint delayed every loading screen, so it is built on first use.
    ui::ChoiceModal *failure() {
        if (failureDialog)
            return failureDialog;
        failureDialog =
            new ui::ChoiceModal(this,
                                {storageFailure ? strings::get("opening.storage_failed_detail")
                                                : strings::get("opening.failed_detail"),
                                 storageFailure ? strings::get("opening.storage_failed") : strings::get("opening.failed"),
                                 {}},
                                {strings::get("opening.go_back"), "rrOpeningBack"},
                                {strings::get("opening.try_native"), "rrOpeningNative"});
        failureDialog->setObjectName("rrOpeningFailure");
        connect(failureDialog->primaryButton(), &QPushButton::clicked, this, &QDialog::accept);
        connect(failureDialog->secondaryButton(), &QPushButton::clicked, this, &QDialog::reject);
        for (auto *widget : failureDialog->findChildren<QWidget *>())
            platform::textRefresh(widget);
        platform::textRefresh(failureDialog);
        return failureDialog;
    }
    void scheduleAfterPaint(std::function<void()> callback) {
        kobalt::qt::later(0, this, [this, callback = std::move(callback)] {
            if (isVisible()) {
                callback();
            }
        });
    }
    qreal scale() const {
        return qMin(width() / ui::metrics::opening::referenceWidth, height() / ui::metrics::opening::referenceHeight);
    }
    QPointF origin() const {
        return {(width() - ui::metrics::opening::referenceWidth * scale()) / 2,
                (height() - ui::metrics::opening::referenceHeight * scale()) / 2};
    }
    QImage cover;
    QLabel *brand, *label, *hint, *detail;
    ui::ChoiceModal *failureDialog = nullptr;
    bool storageFailure = false;
    bool fallback = false, automatic = false, protectedIcon = false;
    std::unique_ptr<WindowPresentation> presentation;
    const bool embedded;
    QTimer hintTimer;
    ui::Button *cancel;
    bool painted = false;
    std::function<void()> firstPaint;
};

class OpeningView final : public ReaderOpeningView {
public:
    OpeningView(QWidget *background, const QJsonObject &preferences, PresentationFactory presentation,
                bool embedded = false)
        : dialog(new OpeningDialog(background, preferences.value("dark_mode").toBool(false), std::move(presentation),
                                   embedded)) {
        if (embedded)
            return;
        auto *screen = QGuiApplication::primaryScreen();
        dialog->setGeometry(screen->geometry());
        QObject::connect(screen, &QScreen::geometryChanged, dialog, [window = dialog](const QRect &geometry) {
            if (window)
                window->setGeometry(geometry);
        });
    }
    ~OpeningView() override {
        if (dialog) {
            dialog->hide();
            dialog->deleteLater();
        }
    }
    QDialog *window() const override {
        return dialog;
    }
    void showFallback(OpeningFallback state) override {
        if (dialog)
            dialog->showFallback(state);
    }
    void setCover(QImage cover) override {
        if (dialog)
            dialog->setCover(std::move(cover));
    }
    void setProgress(int percent) override {
        if (!dialog)
            return;
        auto *bar = dialog->progress;
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
    void afterFirstPaint(std::function<void()> callback) override {
        if (dialog) {
            dialog->afterFirstPaint(std::move(callback));
        }
    }
    void refreshFonts() override {
        if (dialog) {
            dialog->refreshFonts();
        }
    }

private:
    QPointer<OpeningDialog> dialog;
};
}
std::unique_ptr<ReaderOpeningView> createOpening(QWidget *background, const QJsonObject &preferences,
                                                 PresentationFactory presentation) {
    return std::make_unique<OpeningView>(background, preferences, std::move(presentation));
}
std::unique_ptr<ReaderOpeningView> createEmbeddedOpening(QWidget *parent, PresentationFactory presentation) {
    QJsonObject preferences;
    preferences["dark_mode"] = ui::darkTheme(parent);
    return std::make_unique<OpeningView>(parent, preferences, std::move(presentation), true);
}
SaveErrorView::SaveErrorView(QWidget *parent, const QString &error, bool retried)
    : ui::ChoiceModal(parent,
          {strings::get("closing.failed_detail") +
               (error.isEmpty() ? QString() : "\n\n" + error) +
               (retried ? "\n\n" + strings::get("closing.quit_warning") : QString()),
           strings::get("closing.failed"), {}},
          {strings::get("closing.return"), "rrCloseSaveReturn"},
          {strings::get("closing.retry"), "rrCloseSaveRetry"}) {
    setObjectName("rrCloseSaveError");
    if (retried) {
        quit = ui::makeModalButton(card(), {strings::get("closing.quit"), "rrCloseSaveQuit"}, false);
        quit->ensurePolished();
        quit->setFixedHeight(ui::metrics::chrome::actionHeight);
        card()->rows()->addWidget(quit);
    }
    fitCard();
}
SavingView::SavingView(QWidget *parent) : ui::ModalOverlay(parent) {
    setObjectName("rrSaving");
    setProperty("reader_standard_modal", true);
    card()->title(strings::get("closing.saving"), "rrSavingTitle");
    card()->message(strings::get("closing.may_take_a_while"), "rrSavingHint");
    progress = new ui::ProgressBar(card());
    progress->setObjectName("rrSavingProgress");
    card()->rows()->addWidget(progress);
    fitCard();
    for (auto *widget : findChildren<QWidget *>())
        platform::textRefresh(widget);
    platform::textRefresh(this);
}
void SavingView::setProgress(int percent) {
    progress->setValue(qBound(0, percent, 100));
}
}
