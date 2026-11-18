#pragma once
#include <QString>
#include <utility>

namespace kobalt::android {
// Copy results can arrive after a different Library request or an external open.
class LibraryOpen
{
public:
    struct Selection
    {
        QString key, position;
    };

    qint64 begin(QString key, QString position)
    {
        requested = { std::move(key), std::move(position) };
        return ++generation;
    }

    Selection complete(qint64 request)
    {
        if (request != generation)
            return {};
        return std::exchange(requested, {});
    }

    void cancel() { requested = {}; }

private:
    Selection requested;
    qint64 generation = 0;
};
} // namespace kobalt::android
