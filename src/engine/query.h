#pragma once
#include <QString>
#include <QtGlobal>
#include <utility>

// Results that an engine gives for values that can depend on layout. An
// engine that lays out one spine item at a time does not know every value
// at once. No state is a magic number, an empty string or a null value.
namespace kobalt {
enum class QueryState {
    ReadyExact, // A valid value for the current layout.
    ReadyEstimated, // A usable estimate. The basis says what it is made from.
    PendingLayout, // Supported, but the layout that it needs does not exist yet.
    Unsupported, // Not available for this engine, book or setting.
    Failed, // The engine met an error.
};

// What an estimate is made from.
enum class QueryBasis { None, Pages, Words, Bytes };

// The layout that a value belongs to. A value from an older layout or
// another session is stale and must not be shown as current.
struct LayoutGeneration
{
    quint64 session = 0, layout = 0;

    bool operator==(const LayoutGeneration &other) const
    {
        return session == other.session && layout == other.layout;
    }

    bool operator!=(const LayoutGeneration &other) const { return !(*this == other); }
};

template <typename T>
class Query
{
public:
    static Query exact(T value, LayoutGeneration generation = {})
    {
        return Query(QueryState::ReadyExact, std::move(value), QueryBasis::None, {}, generation);
    }

    static Query estimated(T value, QueryBasis basis, LayoutGeneration generation = {})
    {
        Q_ASSERT(basis != QueryBasis::None);
        return Query(QueryState::ReadyEstimated, std::move(value), basis, {}, generation);
    }

    static Query pending(LayoutGeneration generation = {})
    {
        return Query(QueryState::PendingLayout, T(), QueryBasis::None, {}, generation);
    }

    // `reason` is text that the interface can show.
    static Query unsupported(QString reason)
    {
        return Query(QueryState::Unsupported, T(), QueryBasis::None, std::move(reason), {});
    }

    static Query failed(QString reason)
    {
        return Query(QueryState::Failed, T(), QueryBasis::None, std::move(reason), {});
    }

    // A new query is pending: nothing is known and nothing went wrong.
    Query() = default;

    QueryState state() const { return state_; }

    // True for an exact value and for an estimate.
    bool ready() const
    {
        return state_ == QueryState::ReadyExact || state_ == QueryState::ReadyEstimated;
    }

    bool exact() const { return state_ == QueryState::ReadyExact; }

    // Only a ready query has a value.
    const T &value() const
    {
        Q_ASSERT(ready());
        return value_;
    }

    QueryBasis basis() const { return basis_; }

    const QString &reason() const { return reason_; }

    LayoutGeneration generation() const { return generation_; }

    // A value of another layout is not current, whatever its state.
    bool currentFor(const LayoutGeneration &generation) const { return generation_ == generation; }

private:
    Query(QueryState state, T value, QueryBasis basis, QString reason, LayoutGeneration generation)
        : state_(state),
          value_(std::move(value)),
          basis_(basis),
          reason_(std::move(reason)),
          generation_(generation)
    {
    }

    QueryState state_ = QueryState::PendingLayout;
    // Meaningful only when ready. The Kobo Qt5 toolchain has no std::optional.
    T value_ = T();
    QueryBasis basis_ = QueryBasis::None;
    QString reason_;
    LayoutGeneration generation_;
};

// Page numbers of the reading position. The values inside the chapter and
// the values over the book are separate: a chapter can have exact pages
// while the book page is pending, because chapters before it have no layout.
// Pages are zero-based.
struct PageInfo
{
    Query<int> chapterPage, chapterPages;
    Query<int> bookPage, bookPages;
    // The part of the book before the position, from 0 to 1.
    Query<double> bookProgress;
};
} // namespace kobalt
