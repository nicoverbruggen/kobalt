#pragma once
#include <QObject>
#include <QString>
#include <functional>
#ifndef KOBALT_READER_HARDWARE
#  include <QJsonObject>
#endif

namespace kobalt {
// Construct only after firmwareSupported() accepts Nickel.
// Native values live only within a GUI-thread check, never across a book session.
class NativeAuthorization : public QObject
{
    Q_OBJECT
public:
    enum class Reason { Subscription, Removed, Account, Borrowed };
    explicit NativeAuthorization(QObject *parent = nullptr);
    ~NativeAuthorization() override;

    bool available() const { return connected_; }

    bool begin(const QString &book);
    void end();
    void setBlocked(bool blocked);
    // Resume must wait for a completed check of the same session.
    bool checkNow();

    bool invalidated() const { return invalidated_; }

    quint64 generation() const { return generation_; }

    std::function<void(Reason)> revoked;
    static const char *name(Reason reason);
    static void notify(Reason reason, const QString &book = {});
public slots:
    void requestCheck();
#ifndef KOBALT_READER_HARDWARE
public:
    struct TestOnly
    {
    };

    explicit NativeAuthorization(TestOnly, QObject *parent = nullptr);
    void testBegin(const QString &book);
    enum class TestCheck { Native, Deny, Unavailable };
    TestCheck testNextCheck = TestCheck::Native;
    static void testNotify(Reason reason, const QString &book, quint64 generation);
    static void testCommand();
#endif
private:
    void consume(Reason reason, const QString &book, quint64 generation);
    QString book_, account_;
    quint64 generation_ = 0;
    quint64 accountCheck_ = 0;
    int accountRetries_ = 0;
    bool connected_ = false, blocked_ = false, pending_ = false;
    bool queued_ = false, checking_ = false, invalidated_ = false;
};

extern void (*nativeMaybeExitBook)(void *, const void *);
extern void (*nativeVolumeRemoved)(void *, const void *);
extern void (*nativeAccountSignOut)(void *);
extern "C" void kobalt_authorization_exit_book(void *, const void *);
extern "C" void kobalt_authorization_volume_removed(void *, const void *);
extern "C" void kobalt_authorization_sign_out(void *);
} // namespace kobalt
