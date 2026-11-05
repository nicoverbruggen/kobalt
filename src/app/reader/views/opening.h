#pragma once
#include "app/reader_interface.h"
#include "ui/dialogs/modals.h"
namespace kobalt {
std::unique_ptr<ReaderOpeningView> createOpening(QWidget *background, const QJsonObject &preferences,
                                                 PresentationFactory presentation);
// The standalone host sizes this loading view and handles system Back itself.
std::unique_ptr<ReaderOpeningView> createEmbeddedOpening(QWidget *parent, PresentationFactory presentation);
class SaveErrorView final : public ui::ChoiceModal {
public:
    SaveErrorView(QWidget *parent, const QString &error, bool retried);
    QPushButton *quitButton() const { return quit; }

private:
    QPushButton *quit = nullptr;
};
// Final-save feedback stays over the reading page and cannot be dismissed.
class SavingView final : public ui::ModalOverlay {
public:
    explicit SavingView(QWidget *parent);
    void setProgress(int percent);

private:
    ui::ProgressBar *progress;
};
}
