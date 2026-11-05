#pragma once

#include "word_estimates.h"

#include <QJsonObject>

#include <cmath>

namespace kobalt {
// Standalone reading history. Store aggregate word/time samples, never book
// identities or passages. Old samples gradually lose weight after 30 minutes.
class ReadingSpeed
{
public:
    static constexpr qint64 learningMilliseconds = 5 * 60 * 1000;
    static constexpr qint64 historyMilliseconds = 30 * 60 * 1000;
    static constexpr double initialWordsPerMinute = 250;

    void restore(const QJsonObject &state)
    {
        if (state.value("schema").toDouble(-1) != 1) {
            return;
        }
        const auto time = state.value("milliseconds");
        const auto count = state.value("words");
        const auto pages = state.value("samples");
        const double ms = time.toDouble(-1), words = count.toDouble(-1),
                     samples = pages.toDouble(-1);
        const double rate = words * 60000 / ms;
        if (!time.isDouble() || !count.isDouble() || !pages.isDouble() || !std::isfinite(ms)
            || !std::isfinite(words) || !std::isfinite(samples) || ms < 2000
            || ms > historyMilliseconds || samples < 1 || samples > 3
            || samples != std::floor(samples) || !std::isfinite(rate) || rate < 40 || rate > 1200) {
            return;
        }
        milliseconds_ = ms;
        words_ = words;
        samples_ = int(samples);
    }

    QJsonObject state() const
    {
        QJsonObject result;
        result["schema"] = 1;
        result["milliseconds"] = milliseconds_;
        result["words"] = words_;
        result["samples"] = samples_;
        return result;
    }

    bool dirty() const { return dirty_; }

    void saved() { dirty_ = false; }

    void pause()
    {
        previous = {};
        observedAt = -1;
    }

    // Call only while the reading page is visible, using cumulative active time.
    // Repeated frames retain the start time; jumps and reflows start a new sample.
    void observe(const ReadingActivity &activity, qint64 activeMilliseconds)
    {
        if (activity.position.isEmpty() || activeMilliseconds < 0) {
            pause();
            return;
        }
        if (observedAt >= 0 && activity.position == previous.position
            && activity.pages == previous.pages && activity.totalWords == previous.totalWords
            && activeMilliseconds >= observedAt) {
            return;
        }
        const qint64 elapsed = observedAt >= 0 ? activeMilliseconds - observedAt : 0;
        const int words = wordEstimates::pageWords(previous);
        const bool forward = activity.turnDirection > 0
                && qint64(activity.page) == qint64(previous.page) + 1
                && activity.pages == previous.pages && activity.totalWords == previous.totalWords;
        previous = activity;
        observedAt = activeMilliseconds;
        if (!forward || elapsed < 2000 || elapsed > learningMilliseconds || words <= 0) {
            return;
        }
        const double rate = double(words) * 60000 / elapsed;
        if (rate < 40 || rate > 1200) {
            return;
        }
        const double retained = qMin(milliseconds_, double(historyMilliseconds - elapsed));
        if (milliseconds_ > 0) {
            words_ *= retained / milliseconds_;
        }
        milliseconds_ = retained + elapsed;
        words_ += words;
        samples_ = qMin(3, samples_ + 1);
        dirty_ = true;
    }

    ReadingEstimates estimates(const ReadingActivity &activity) const
    {
        // A fixed starting estimate fades out as accepted reading accumulates.
        const double prior = qMax(0.0, double(learningMilliseconds) - milliseconds_);
        const double secondsPerWord =
                (milliseconds_ + prior) / (words_ + prior * initialWordsPerMinute / 60000) / 1000;
        auto result = wordEstimates::readingEstimates(activity, secondsPerWord);
        result.learningPace = result.bookMilliseconds >= 0
                && (milliseconds_ < learningMilliseconds || samples_ < 3);
        return result;
    }

private:
    double milliseconds_ = 0, words_ = 0;
    int samples_ = 0;
    bool dirty_ = false;
    ReadingActivity previous;
    qint64 observedAt = -1;
};
} // namespace kobalt
