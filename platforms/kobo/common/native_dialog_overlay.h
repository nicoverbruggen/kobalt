#pragma once

#include <QApplication>
#include <QEvent>
#include <QPointer>
#include <QWidget>
#include <QVariant>
#include <QLineEdit>
#include <QWindow>
#include <QLabel>
#include "support/qt_compat.h"

namespace kobalt::nickel {
// Nickel keeps ownership and styling. Temporarily give its dialog a window
// above the reader, then restore the native flags when it closes.
class NativeDialogOverlay : public QObject
{
public:
    NativeDialogOverlay() { qApp->installEventFilter(this); }

    ~NativeDialogOverlay() override { restore(); }

    bool active() const { return !dialog_.isNull(); }

    static bool activeFor(const QWidget *reader)
    {
        return reader && reader->window()->property("kobalt_native_dialog_count").toInt() > 0;
    }

    static bool inputPending()
    {
        for (auto *widget : QApplication::allWidgets()) {
            if (!widget->isVisible() || !widget->property("kobalt_native_dialog_overlay").toBool())
                continue;
            if (widget->inherits("ConfirmationDialog") || widget->findChild<QLineEdit *>())
                return true;
            for (auto *child : widget->findChildren<QWidget *>())
                if (child->inherits("WirelessNetworksView"))
                    return true;
        }
        return false;
    }

    bool contains(const QWidget *widget) const
    {
        return dialog_ && widget && (widget == dialog_ || dialog_->isAncestorOf(widget));
    }

    void present(QWidget *dialog, QWidget *reader)
    {
        if (!dialog || !reader || active()) {
            return;
        }
        dialog_ = dialog;
        dialog->setProperty("kobalt_native_dialog_overlay", true);
        connect(dialog, &QObject::destroyed, this, [this] {
            dialog_.clear();
            releaseReader();
            parent_.clear();
        });
        reader_ = reader;
        reader_->setProperty("kobalt_native_dialog_count",
                             reader_->property("kobalt_native_dialog_count").toInt() + 1);
        parent_ = dialog->parentWidget();
        flags_ = dialog->windowFlags();
        modality_ = dialog->windowModality();
        geometry_ = dialog->geometry();
        propagation_ = dialog->testAttribute(Qt::WA_WindowPropagation);
        transient_ = dialog->windowHandle() ? dialog->windowHandle()->transientParent() : nullptr;
        const auto position = dialog->mapToGlobal(QPoint());
        const auto background =
                reader->grab(QRect(reader->mapFromGlobal(position), dialog->size()));
        moving_ = true;
        // Keep Nickel's widget ancestry, fonts and style sheets. Only the
        // window relationship changes so the dialog can paint above Kobalt.
        dialog->setAttribute(Qt::WA_WindowPropagation, true);
        dialog->setWindowFlags((flags_ & ~Qt::WindowType_Mask) | Qt::Dialog
                               | Qt::FramelessWindowHint);
        dialog->move(position);
        dialog->winId();
        dialog->windowHandle()->setTransientParent(reader->windowHandle());
        // A transient parent puts the dialog above Kobalt visually, but a
        // nonmodal window still loses input to the application-modal reader.
        if (modality_ == Qt::NonModal)
            dialog->setWindowModality(Qt::WindowModal);
        dialog->setUpdatesEnabled(true);
        // The device's backing store does not composite transparent windows.
        // Retain one copy of the covered reader area until this modal closes.
        backdrop_ = new QLabel(dialog);
        backdrop_->setAttribute(Qt::WA_TransparentForMouseEvents);
        backdrop_->setGeometry(dialog->rect());
        backdrop_->setPixmap(background);
        backdrop_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        backdrop_->lower();
        backdrop_->show();
        dialog->show();
        dialog->raise();
        moving_ = false;
    }

    void restore()
    {
        if (!dialog_) {
            return;
        }
        moving_ = true;
        auto dialog = dialog_;
        dialog_.clear();
        dialog->setProperty("kobalt_native_dialog_overlay", QVariant());
        releaseReader();
        dialog->hide();
        delete backdrop_.data();
        dialog->setParent(parent_, flags_);
        dialog->setWindowModality(modality_);
        dialog->setAttribute(Qt::WA_WindowPropagation, propagation_);
        if (dialog->windowHandle())
            dialog->windowHandle()->setTransientParent(transient_);
        dialog->setGeometry(geometry_);
        parent_.clear();
        reader_.clear();
        moving_ = false;
    }

private:
    void releaseReader()
    {
        if (!reader_)
            return;
        const int count = reader_->property("kobalt_native_dialog_count").toInt() - 1;
        reader_->setProperty("kobalt_native_dialog_count",
                             count > 0 ? QVariant(count) : QVariant());
        reader_.clear();
    }

    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (!moving_ && event->type() == QEvent::Hide && object == dialog_) {
            // QWidget also hides during destruction, after removing its focus
            // chain. Changing window flags there would touch that dead chain.
            const QPointer<QWidget> hidden = dialog_;
            qt::later(0, this, [this, hidden] {
                if (hidden && hidden == dialog_ && !hidden->isVisible())
                    restore();
            });
        } else if (!moving_
                   && ((event->type() == QEvent::Hide && object == reader_)
                       || ((event->type() == QEvent::Close || event->type() == QEvent::Destroy)
                           && object == reader_))) {
            restore();
        }
        return false;
    }

    QPointer<QWidget> dialog_, reader_, parent_;
    QPointer<QLabel> backdrop_;
    QPointer<QWindow> transient_;
    Qt::WindowFlags flags_;
    Qt::WindowModality modality_ = Qt::NonModal;
    QRect geometry_;
    bool moving_ = false;
    bool propagation_ = false;
};
} // namespace kobalt::nickel
