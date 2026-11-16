#pragma once
#include "app/reader_interface.h"
#include "ui/dialogs/modals.h"

namespace kobalt {
std::unique_ptr<ReaderOpeningView> createOpening(QWidget *background,
                                                 const QJsonObject &preferences,
                                                 PresentationFactory presentation);
// The standalone host sizes this loading view and handles system Back itself.
std::unique_ptr<ReaderOpeningView>
createEmbeddedOpening(QWidget *parent, PresentationFactory presentation, bool showCover = true);
// Paint the centered logo without fonts, images or controls until prepareControls().
std::unique_ptr<ReaderOpeningView> createOpeningSurface(QWidget *parent, bool dark,
                                                        PresentationFactory presentation);

class SaveErrorView final : public ui::ChoiceModal
{
public:
    // A failed reading-time save can be retried while the reader stays open.
    explicit SaveErrorView(QWidget *parent);
    enum class Cause { Failed, TimedOut };
    // `savedLocally`: a timed-out close whose own saves are done, so only the
    // host's records wait. It offers to close without waiting for them.
    SaveErrorView(QWidget *parent, const QString &error, bool retried, Cause cause = Cause::Failed,
                  bool savedLocally = false);

    QPushButton *quitButton() const { return quit; }

private:
    QPushButton *quit = nullptr;
};

} // namespace kobalt
