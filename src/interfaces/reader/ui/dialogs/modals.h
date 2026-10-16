#pragma once
#include "../controls/panels.h"
#include <QBoxLayout>
#include <QDialog>
#include <QImage>
#include <QPixmap>
#include <QPushButton>
#include <functional>

namespace kobalt::ui {
// All centered notices share this card, including nonblocking feedback.
class PopupCard : public Panel {
public:
    explicit PopupCard(QWidget *parent);
    QVBoxLayout *rows() const {
        return contentRows;
    }
    QLabel *title(const QString &text, const QString &name = {});
    QLabel *message(const QString &text, const QString &name = {});
    QHBoxLayout *actions();
    void image(const QImage &image);
    void adaptiveActions(const QVector<QPushButton *> &buttons);
    void fitToWidth(int width);

private:
    QVBoxLayout *contentRows;
    QBoxLayout *actionRows = nullptr;
    QVector<QPushButton *> actionButtons;
    QLabel *artwork = nullptr;
    QImage artworkImage;
};

// The transparent input barrier keeps the reading page unchanged. Only the
// opaque card needs a refresh when a modal appears or disappears.
class ModalOverlay : public QWidget {
public:
    explicit ModalOverlay(QWidget *parent);
    PopupCard *card() const {
        return content;
    }
    void centerCard();
    void fitCard();
    void setDelayedProgress(bool active, qint64 elapsedMilliseconds, int percent, const QString &message = {});
    // Window coordinates for a value-only update of the visible progress bar.
    // Appearance, message and layout changes keep the modal's normal refresh.
    QRect progressUpdateRect(int percent, const QString &message = {}) const;
    void anchorTo(const QRect &bounds) {
        anchor = bounds;
        centerCard();
    }
    std::function<void()> dismissed;

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void resizeEvent(QResizeEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool focusNextPrevChild(bool next) override;

private:
    PopupCard *content;
    QWidget *pointer;
    QRect anchor;
    ProgressBar *loadingProgress = nullptr;
    QLabel *loadingMessage = nullptr;
};

struct ModalContent {
    QString description;
    QString title;
    QImage image;
};
struct ModalAction {
    QString text;
    QString name;
};
QPushButton *makeModalButton(QWidget *parent, const ModalAction &action, bool primary);
class NoticeModal final : public ModalOverlay {
public:
    NoticeModal(QWidget *parent, const ModalContent &content, const ModalAction &action);
    QLabel *descriptionLabel() const {
        return description;
    }
    QPushButton *primaryButton() const {
        return primary;
    }

private:
    QLabel *description;
    QPushButton *primary;
};
class ChoiceModal : public ModalOverlay {
public:
    ChoiceModal(QWidget *parent, const ModalContent &content, const ModalAction &secondary, const ModalAction &primary);
    QLabel *descriptionLabel() const {
        return description;
    }
    QPushButton *primaryButton() const {
        return primary;
    }
    QPushButton *secondaryButton() const {
        return secondary;
    }

private:
    QLabel *description;
    QPushButton *primary;
    QPushButton *secondary;
};

// Opening a book has no Reader parent yet. This modal host captures the
// library only in RAM and uses the same overlay and card as in-reader alerts.
class PopupWindow : public QDialog {
public:
    explicit PopupWindow(QWidget *background, bool dark = false);
    PopupCard *card() const {
        return overlay->card();
    }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QPixmap background;
    ModalOverlay *overlay;
};
}
