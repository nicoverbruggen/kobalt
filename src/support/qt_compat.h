#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QObject>
#include <QTimer>
#include <initializer_list>
#include <utility>

#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
using qsizetype = qptrdiff;
#endif

#if QT_VERSION < QT_VERSION_CHECK(5, 5, 0)
// Qt 5.2 has no informational message stream.
#  define qInfo qDebug
#endif

namespace kobalt::qt {
inline QString character(char32_t codepoint)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QString::fromUcs4(&codepoint, 1);
#else
    const uint value = codepoint;
    return QString::fromUcs4(&value, 1);
#endif
}

class SignalBlocker
{
public:
    explicit SignalBlocker(QObject *object)
        : object(object), blocked(object->blockSignals(true)) { }

    ~SignalBlocker() { object->blockSignals(blocked); }

    SignalBlocker(const SignalBlocker &) = delete;
    SignalBlocker &operator=(const SignalBlocker &) = delete;

private:
    QObject *object;
    bool blocked;
};

#if QT_VERSION < QT_VERSION_CHECK(5, 14, 0)
constexpr auto skipEmptyParts = QString::SkipEmptyParts;
#else
constexpr auto skipEmptyParts = Qt::SkipEmptyParts;
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
inline QJsonValue jsonValue(qint64 value)
{
    return QJsonValue(double(value));
}

inline QJsonValue jsonValue(quint64 value)
{
    return QJsonValue(double(value));
}
#endif
inline QJsonValue jsonValue(const char *value)
{
    return QJsonValue(QString::fromUtf8(value));
}

template <class Value>
QJsonValue jsonValue(const Value &value)
{
    return QJsonValue(value);
}

struct JsonField
{
    QString key;
    QJsonValue value;

    template <class Value>
    JsonField(QString key, const Value &value) : key(std::move(key)), value(jsonValue(value))
    {
    }
};

inline QJsonObject object(std::initializer_list<JsonField> fields)
{
    QJsonObject result;
    for (const auto &field : fields)
        result.insert(field.key, field.value);
    return result;
}

inline QJsonArray array(std::initializer_list<QJsonValue> values)
{
    QJsonArray result;
    for (const auto &value : values)
        result.append(value);
    return result;
}

template <class Function>
bool invoke(QObject *context, Function function, Qt::ConnectionType type = Qt::AutoConnection)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
    return QMetaObject::invokeMethod(context, std::move(function), type);
#else
    if (!context)
        return false;
    // Qt5.2 has functor connections but no functor invokeMethod. A short-lived
    // sender queues the call to the receiver and preserves its lifetime guard.
    QObject sender;
    return bool(QObject::connect(&sender, &QObject::destroyed, context, std::move(function), type));
#endif
}

template <class Function>
void later(int milliseconds, QObject *context, Function function)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 4, 0)
    QTimer::singleShot(milliseconds, context, std::move(function));
#else
    auto *timer = new QTimer(context);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, context, std::move(function));
    QObject::connect(timer, &QTimer::timeout, timer, &QObject::deleteLater);
    timer->start(milliseconds);
#endif
}
} // namespace kobalt::qt
