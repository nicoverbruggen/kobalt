#pragma once
#include "ui/controls/panels.h"
#include <QBoxLayout>
#include <QDialog>
#include <QImage>
#include <QPixmap>
#include <QRegion>
#include <QPushButton>
#include <QPointer>
#include <functional>

namespace kobalt::ui {
class PagerBar;

// All centered notices share this card, including nonblocking feedback.
class PopupCard : public Panel
{
public:
    explicit PopupCard(QWidget *parent);

    QVBoxLayout *rows() const { return contentRows; }

    QLabel *title(const QString &text, const QString &name = {});
    QLabel *message(const QString &text, const QString &name = {});
    QHBoxLayout *actions();
    void image(const QImage &image);
    void adaptiveActions(const QVector<QPushButton *> &buttons);
    void fitToWidth(int width);
    // A card taller than `height`, as on a phone in landscape, keeps its first
    // row and its actions and pages the rows between them. Call after
    // fitToWidth(); a card that fits shows every row again.
    void fitToHeight(int height);

private:
    QVBoxLayout *contentRows;
    PagerBar *rowPager = nullptr;
    QVector<QVector<QWidget *>> rowPages;
    QVector<QPointer<QWidget>> pagedRows;
    int rowPage = 0;
    void showRowPage(int page);
    QBoxLayout *actionRows = nullptr;
    QVector<QPushButton *> actionButtons;
    QLabel *artwork = nullptr;
    QImage artworkImage;
};

// The input barrier can pattern the background for settings choices. Transparent
// overlays keep local refreshes for progress and anchored reading tools.
class ModalOverlay : public QWidget
{
public:
    explicit ModalOverlay(QWidget *parent);
    void setVisible(bool visible) override;

    PopupCard *card() const { return content; }

    // Nonblocking anchored menus receive input only over their card and arrow.
    // Page gestures and controls outside those pixels remain available.
    void setInputBarrier(bool enabled);
    // Keep underlying adjustment handles usable without dismissing the menu.
    void setInputExclusions(const QRegion &region);
    // The patterned backdrop covers the parent only while this overlay blocks input.
    void setDimmed(bool enabled);

    bool inputBarrier() const { return blocksInput; }

    void centerCard();

    // Restrict text-entry cards to the part of the screen above the keyboard.
    void setCardArea(const QRect &area)
    {
        cardArea = area;
        centerCard();
    }

    void fitCard();
    void setDelayedProgress(bool active, qint64 elapsedMilliseconds, int percent,
                            const QString &message = {});
    // Window coordinates for a value-only update of the visible progress bar.
    // Appearance, message and layout changes keep the modal's normal refresh.
    QRect progressUpdateRect(int percent, const QString &message = {}) const;

    void anchorTo(const QRect &bounds)
    {
        anchor = bounds;
        centerCard();
    }

    std::function<void()> dismissed;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void updateInputRegion();
    bool blocksInput = true, dimmed = false;
    PopupCard *content;
    QWidget *pointer;
    QRect anchor, cardArea;
    QRegion inputExclusions;
    ProgressBar *loadingProgress = nullptr;
    QLabel *loadingMessage = nullptr;
};

struct ModalContent
{
    QString description;
    QString title;
    QImage image;
};

struct ModalAction
{
    QString text;
    QString name;
};

QPushButton *makeModalButton(QWidget *parent, const ModalAction &action, bool primary);

class NoticeModal : public ModalOverlay
{
public:
    NoticeModal(QWidget *parent, const ModalContent &content, const ModalAction &action);

    QLabel *descriptionLabel() const { return description; }

    QPushButton *primaryButton() const { return primary; }

private:
    QLabel *description;
    QPushButton *primary;
};

class ChoiceModal : public ModalOverlay
{
public:
    ChoiceModal(QWidget *parent, const ModalContent &content, const ModalAction &secondary,
                const ModalAction &primary);

    QLabel *descriptionLabel() const { return description; }

    QPushButton *primaryButton() const { return primary; }

    QPushButton *secondaryButton() const { return secondary; }

private:
    QLabel *description;
    QPushButton *primary;
    QPushButton *secondary;
};

// Opening a book has no Reader parent yet. This modal host captures the
// library only in RAM and uses the same overlay and card as in-reader alerts.
class PopupWindow : public QDialog
{
public:
    explicit PopupWindow(QWidget *background, bool dark = false);

    PopupCard *card() const { return overlay->card(); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QPixmap background;
    ModalOverlay *overlay;
};
} // namespace kobalt::ui
