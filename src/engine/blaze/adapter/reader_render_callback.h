#pragma once
#include "engine_document.h"
#include "engine/engine_activity.h"
#include "column_view.h"

namespace kobalt {
// The engine retains a raw callback pointer. Restore the previous callback
// on the same view, even when a chapter change moves its unique owner.
class OpeningCallback : public LVDocViewCallback
{
public:
    OpeningCallback(std::unique_ptr<ColumnView> &engine, OpeningProgress progress,
                    std::function<bool()> interrupt = {})
        : target(engine->callbackTarget()),
          progress(std::move(progress)),
          interrupt(std::move(interrupt))
    {
        previous = engine->setCallback(this);
    }

    ~OpeningCallback() override
    {
        if (auto *view = target->view; view && view->getCallback() == this)
            view->setCallback(previous);
    }

    OpeningCallback(const OpeningCallback &) = delete;
    OpeningCallback &operator=(const OpeningCallback &) = delete;

    void OnLoadFileProgress(int percent) override { report(OpeningStage::Text, percent); }

    void OnNodeStylesUpdateStart() override { report(OpeningStage::Styles, 0); }

    void OnNodeStylesUpdateProgress(int percent) override { report(OpeningStage::Styles, percent); }

    void OnFormatStart() override { report(OpeningStage::Pages, 0); }

    void OnFormatProgress(int percent) override { report(OpeningStage::Pages, percent); }

#if __has_include(<koblaze.h>)
    // Asked after each block of text during layout.
    bool OnRenderCancelRequested() override { return interrupt && interrupt(); }
#endif

private:
    std::shared_ptr<ColumnView::CallbackTarget> target;
    LVDocViewCallback *previous = nullptr;
    OpeningProgress progress;
    std::function<bool()> interrupt;

    void report(OpeningStage stage, int percent)
    {
        engineActivity().checkpoint();
        if (progress)
            progress(stage, qBound(0, percent, 100));
    }
};
} // namespace kobalt
