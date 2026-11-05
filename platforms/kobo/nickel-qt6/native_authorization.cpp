#include "support/qt_compat.h"
#include "platforms/kobo/common/native_authorization.h"
#include "support/diagnostics.h"
#include <QCoreApplication>
#include <QMetaType>
#include <QPointer>
#include <QThread>
#include <QSqlDatabase>
#include <atomic>
#include <dlfcn.h>
#ifndef KOBALT_READER_HARDWARE
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#endif

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
struct Volume {
    void *words[3];
    Volume(const Volume &) = delete;
    ~Volume() {
        symbol<void (*)(void *)>("_ZN6VolumeD1Ev")(this);
    }
};
// getUserOnDevice returns an eight-byte User value in the supported ABI.
struct User {
    void *words[2];
    User(const User &) = delete;
    ~User() {
        symbol<void (*)(void *)>("_ZN4UserD1Ev")(this);
    }
};
static_assert(sizeof(Volume) == 12 && sizeof(User) == 8);
struct Api {
    Volume (*get)(const QString &, const QString &) = symbol<decltype(get)>("_ZN13VolumeManager7getByIdERK7QStringS2_");
    QString (*id)(const void *, const QString &) = symbol<decltype(id)>("_ZNK7Content5getIdERK7QString");
    void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    const QString &(*dbName)(const void *) = symbol<decltype(dbName)>("_ZNK6Device9getDbNameEv");
    QSqlDatabase (*connection)(const QString &,
                               bool) = symbol<decltype(connection)>("_ZN15DatabaseAdapter12dbConnectionERK7QStringb");
    User (*user)(const QString &) = symbol<decltype(user)>("_ZN11UserManager15getUserOnDeviceERK7QString");
    QString (*userId)(const User *) = symbol<decltype(userId)>("_ZNK4User2idEv");
    QString (*warning)(const Volume &) =
        symbol<decltype(warning)>("_ZN27SubscriptionWorkflowManager14getSyncWarningERK6Volume");
    bool (*borrowed)(const void *) = symbol<decltype(borrowed)>("_ZNK6Volume10isBorrowedEv");
    bool (*openBorrowed)(const Volume &) =
        symbol<decltype(openBorrowed)>("_ZN24OverDriveWorkflowManager8openBookERK6Volume");
    QObject *(*volumes)() = symbol<decltype(volumes)>("_ZN13VolumeManager14sharedInstanceEv");
    QObject *(*sync)() = symbol<decltype(sync)>("_ZN13N3SyncManager14sharedInstanceEv");
    bool ready() const {
        const auto type = QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(
            "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE"));
        return type.sizeOf() == sizeof(Volume) && get && id && device && dbName && connection && user && userId &&
               warning && borrowed && openBorrowed && volumes && sync && symbol<void *>("_ZN6VolumeD1Ev") &&
               symbol<void *>("_ZN4UserD1Ev") && nativeMaybeExitBook && nativeVolumeRemoved && nativeAccountSignOut;
    }
    bool databaseOpen() const {
        const auto db = connection(dbName(device()), false);
        return db.isValid() && db.isOpen();
    }
    QString account() const {
        const auto value = user(dbName(device()));
        return userId(&value);
    }
};
const Api &api() {
    static const Api value;
    return value;
}
QPointer<NativeAuthorization> active;
std::atomic<quint64> activeGeneration{0};
std::atomic_bool watching{false};
QString identity(const void *volume) {
    if (!watching.load())
        return {};
    const auto getId = api().id;
    return getId && volume ? getId(volume, QString()) : QString();
}
}
void (*nativeMaybeExitBook)(void *, const void *);
void (*nativeVolumeRemoved)(void *, const void *);
void (*nativeAccountSignOut)(void *);
extern "C" __attribute__((visibility("default"))) void kobalt_authorization_exit_book(void *manager,
                                                                                      const void *volume) {
    NativeAuthorization::notify(NativeAuthorization::Reason::Subscription, identity(volume));
    nativeMaybeExitBook(manager, volume);
}
extern "C" __attribute__((visibility("default"))) void kobalt_authorization_volume_removed(void *manager,
                                                                                           const void *volume) {
    NativeAuthorization::notify(NativeAuthorization::Reason::Removed, identity(volume));
    nativeVolumeRemoved(manager, volume);
}
extern "C" __attribute__((visibility("default"))) void kobalt_authorization_sign_out(void *mixin) {
    // This native method starts SignOutWorker after the user's confirmation.
    NativeAuthorization::notify(NativeAuthorization::Reason::Account);
    nativeAccountSignOut(mixin);
}
NativeAuthorization::NativeAuthorization(QObject *parent) : QObject(parent) {
    if (!api().ready())
        return;
    const auto a = connect(api().sync(), SIGNAL(finished()), this, SLOT(requestCheck()));
    const auto b = connect(api().sync(), SIGNAL(userProfileUpdated()), this, SLOT(requestCheck()));
    const auto c = connect(api().volumes(), SIGNAL(volumeChanged(Volume)), this, SLOT(requestCheck()));
    connected_ = a && b && c;
}
NativeAuthorization::~NativeAuthorization() {
    end();
}
bool NativeAuthorization::begin(const QString &book) {
    end();
    if (!connected_ || book.isEmpty() || !api().databaseOpen())
        return false;
    account_ = api().account();
    book_ = book;
    generation_ = ++activeGeneration;
    active = this;
    watching.store(true);
    return true;
}
void NativeAuthorization::end() {
    if (active == this) {
        watching.store(false);
        active.clear();
        ++activeGeneration;
    }
    book_.clear();
    account_.clear();
    pending_ = queued_ = invalidated_ = false;
    generation_ = 0;
#ifndef KOBALT_READER_HARDWARE
    testNextCheck = TestCheck::Native;
#endif
}
const char *NativeAuthorization::name(Reason reason) {
    switch (reason) {
    case Reason::Subscription:
        return "subscription";
    case Reason::Removed:
        return "removed";
    case Reason::Account:
        return "account";
    case Reason::Borrowed:
        return "borrowed";
    }
    return "unknown";
}
void NativeAuthorization::setBlocked(bool blocked) {
    blocked_ = blocked;
    if (!blocked_ && pending_)
        requestCheck();
}
void NativeAuthorization::notify(Reason reason, const QString &book) {
    if (!watching.load())
        return;
    auto *app = QCoreApplication::instance();
    if (!app)
        return;
    const auto epoch = activeGeneration.load();
    const auto deliver = [reason, book, epoch] {
        if (active)
            active->consume(reason, book, epoch);
    };
    if (QThread::currentThread() == app->thread())
        deliver();
    else
        kobalt::qt::invoke(app, deliver, Qt::QueuedConnection);
}
void NativeAuthorization::consume(Reason reason, const QString &book, quint64 epoch) {
    if (!generation_ || generation_ != epoch || invalidated_ || book_.isEmpty() ||
        (reason != Reason::Account && book != book_))
        return;
    invalidated_ = true;
    pending_ = false;
    diagnostic("authorization-invalidated", {{"reason", name(reason)}});
    // The owner must hide/cancel immediately, then drain and save locally.
    // In particular it must not write progress back to a revoked native Volume.
    const auto callback = revoked;
    if (callback)
        callback(reason);
}
void NativeAuthorization::requestCheck() {
    if (book_.isEmpty() || invalidated_)
        return;
    pending_ = true;
    if (blocked_ || queued_ || checking_)
        return;
    queued_ = true;
    const auto epoch = generation_;
    kobalt::qt::invoke(
        this,
        [this, epoch] {
            if (epoch != generation_)
                return;
            queued_ = false;
            if (blocked_ || invalidated_ || !pending_)
                return;
            pending_ = false;
            checkNow();
        },
        Qt::QueuedConnection);
}
bool NativeAuthorization::checkNow() {
    if (QThread::currentThread() != thread() || blocked_ || invalidated_ || !generation_ || !connected_ ||
        !api().ready() || book_.isEmpty() || checking_)
        return false;
    if (!api().databaseOpen()) {
        pending_ = true;
        diagnostic("authorization-check-deferred");
        return false;
    }
#ifndef KOBALT_READER_HARDWARE
    const auto testCheck = testNextCheck;
    testNextCheck = TestCheck::Native;
    if (testCheck == TestCheck::Unavailable)
        return false;
    if (testCheck == TestCheck::Deny) {
        consume(Reason::Borrowed, book_, generation_);
        return false;
    }
#endif
    checking_ = true;
    const auto epoch = generation_;
    const auto &native = api();
    if (native.account() != account_)
        consume(Reason::Account, {}, epoch);
    if (!invalidated_ && epoch == generation_) {
        const auto volume = native.get(book_, native.dbName(native.device()));
        if (native.id(&volume, QString()) == book_) {
            // This runs Nickel's status, grace-period and warning logic. Its
            // maybeExitBook hook forwards the cancellation decision to Kobalt.
            native.warning(volume);
            if (!invalidated_ && epoch == generation_ && native.borrowed(&volume) && !native.openBorrowed(volume))
                consume(Reason::Borrowed, book_, epoch);
        } else
            consume(Reason::Removed, book_, epoch);
    }
    checking_ = false;
    const bool allowed = epoch == generation_ && !invalidated_ && !book_.isEmpty();
    diagnostic("authorization-checked", {{"allowed", allowed}});
    // Notifications produced by the check itself need no second check.
    pending_ = false;
    return allowed;
}
#ifndef KOBALT_READER_HARDWARE
NativeAuthorization::NativeAuthorization(TestOnly, QObject *parent) : QObject(parent) {}
void NativeAuthorization::testBegin(const QString &book) {
    end();
    book_ = book;
    generation_ = ++activeGeneration;
    active = this;
    watching.store(true);
}
void NativeAuthorization::testNotify(Reason reason, const QString &book, quint64 epoch) {
    if (active)
        active->consume(reason, book, epoch);
}
void NativeAuthorization::testCommand() {
    QFile file("/instance/kobalt-authorization-command.json");
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return;
    const auto command = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    file.remove();
    const auto event = command.value("event").toString();
    const auto epoch = activeGeneration.load();
    QJsonObject result =
        kobalt::qt::object({{"id", command.value("id")}, {"simulated", true}, {"active", bool(active)}});
    if (active) {
        const auto book = command.value("other_book").toBool() ? "kobalt-test-other" : active->book_;
        const auto target = command.value("stale").toBool() ? epoch - 1 : epoch;
        if (event == "check") {
            result["allowed"] = active->checkNow();
            result["simulated"] = false;
        } else if (event == "deny-next-check" || event == "unavailable-next-check") {
            active->testNextCheck = event == "deny-next-check" ? TestCheck::Deny : TestCheck::Unavailable;
            result["armed"] = true;
        } else if (event == "removed" || event == "account" || event == "borrowed" || event == "subscription") {
            Reason reason = event == "removed"    ? Reason::Removed
                            : event == "account"  ? Reason::Account
                            : event == "borrowed" ? Reason::Borrowed
                                                  : Reason::Subscription;
            active->consume(reason, book, target);
        }
        result["invalidated"] = !active || active->invalidated_;
    }
    QSaveFile report("/instance/artifacts/kobalt-authorization-result.json");
    if (report.open(QIODevice::WriteOnly)) {
        report.write(QJsonDocument(result).toJson());
        report.commit();
    }
}
#endif
}
