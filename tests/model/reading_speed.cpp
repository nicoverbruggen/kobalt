#include "reading/reading_speed.h"
#include "check.h"

#include <QJsonDocument>

#include <cstdio>
#include <limits>

using namespace kobalt;

namespace {
ReadingActivity page(int number, int pages = 100, qint64 words = 30000)
{
    ReadingActivity result;
    result.position = QString::number(number);
    result.page = number;
    result.pages = pages;
    result.totalWords = words;
    result.turnDirection = 1;
    return result;
}

void unchanged(const ReadingSpeed &speed, const QJsonObject &before, const char *message)
{
    require(speed.state() == before, message);
}
} // namespace

int main()
{
    ReadingSpeed speed;
    require(speed.estimates(page(0)).learningPace
                    && speed.estimates(page(0)).bookMilliseconds == 120 * 60000,
            "Cold estimate is not the rough 250 words/minute starting pace");
    speed.observe(page(0), 0);
    speed.observe(page(1), 100000);
    const auto partial = speed.state();
    require(speed.dirty() && partial.value("samples").toInt() == 1,
            "First accepted sample was not recorded");
    speed.saved();
    require(!speed.dirty(), "Saved history remains dirty");
    ReadingSpeed resumed;
    resumed.restore(QJsonDocument::fromJson(QJsonDocument(partial).toJson()).object());
    require(resumed.estimates(page(0)).bookMilliseconds == speed.estimates(page(0)).bookMilliseconds
                    && resumed.estimates(page(0)).learningPace && !resumed.dirty(),
            "Partial history was lost across sessions");
    resumed.observe(page(0), 0);
    resumed.observe(page(1), 100000);
    resumed.pause();
    resumed.observe(page(2), 900000);
    resumed.observe(page(3), 1000000);
    require(!resumed.estimates(page(0)).learningPace
                    && resumed.estimates(page(0)).bookMilliseconds == 10000000,
            "Three accepted pages across sessions did not replace the starting pace");
    ReadingSpeed learned;
    learned.restore(resumed.state());
    require(!learned.estimates(page(0)).learningPace
                    && learned.estimates(page(0)).bookMilliseconds == 10000000,
            "Learned pace was not restored");
    const auto history = learned.state();
    learned.observe(page(0), 0);
    learned.observe(page(1), 1000);
    learned.observe(page(2), 2000);
    unchanged(learned, history, "Rapid paging changed history");
    learned.observe(page(10), 102000);
    learned.observe(page(9), 202000);
    unchanged(learned, history, "Jump or backwards paging changed history");
    learned.observe(page(10), 600000);
    unchanged(learned, history, "Long idle time changed history");
    learned.observe(page(11, 110), 700000);
    unchanged(learned, history, "Reflow changed history");
    learned.pause();
    learned.observe(page(12, 110), 900000);
    unchanged(learned, history, "Pause counted a partial page");
    learned.observe(page(13, 110), 0);
    unchanged(learned, history, "Clock going backwards changed history");
    learned.observe(page(14, 110, -1), 100000);
    unchanged(learned, history, "Missing word counts changed history");
    require(learned.estimates(page(0, 100, -1)).bookMilliseconds == -1,
            "Missing word counts invented an estimate");
    ReadingSpeed duplicates;
    duplicates.observe(page(0), 0);
    duplicates.observe(page(0), 50000);
    duplicates.observe(page(1), 100000);
    unchanged(duplicates, partial, "Repeated frames reset the page's reading time");
    for (const auto &invalid :
         { QJsonObject(), QJsonObject{ { "schema", 2 } }, QJsonObject{ { "schema", 1.5 } } }) {
        ReadingSpeed fresh;
        fresh.restore(invalid);
        require(fresh.estimates(page(0)).bookMilliseconds == 120 * 60000,
                "Invalid schema accepted");
    }
    for (const auto *key : { "milliseconds", "words", "samples" }) {
        for (double value : { -1.0, 0.0, 1e100, std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN() }) {
            auto corrupt = history;
            corrupt[key] = value;
            ReadingSpeed fresh;
            fresh.restore(corrupt);
            require(fresh.estimates(page(0)).bookMilliseconds == 120 * 60000,
                    "Corrupt persisted history replaced the default");
        }
        auto corrupt = history;
        corrupt[key] = "1000";
        ReadingSpeed fresh;
        fresh.restore(corrupt);
        require(fresh.estimates(page(0)).bookMilliseconds == 120 * 60000,
                "String history was accepted");
    }
    ReadingSpeed capped;
    capped.observe(page(0), 0);
    for (int i = 1; i <= 60; ++i) {
        capped.observe(page(i), i * 100000);
    }
    require(capped.state().value("milliseconds").toDouble() == ReadingSpeed::historyMilliseconds,
            "History exceeded its thirty-minute bound");
    require(capped.estimates(page(0)).bookMilliseconds == 10000000,
            "History cap changed a constant pace");
    for (int i = 61; i <= 90; ++i) {
        capped.observe(page(i), 6000000 + (i - 60) * 60000);
    }
    require(capped.estimates(page(0)).bookMilliseconds < 8000000,
            "Old history prevented learning a faster pace");
    std::puts(
            "PASS: rough estimates, persisted partial/learned pace, sample filtering and bounded adaptation");
}
