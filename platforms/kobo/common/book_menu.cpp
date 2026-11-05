#include "book_menu.h"
#include "book_protection.h"
#include "local_book_path.h"
#include "publication/publication.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include <QAction>
#include <QCoreApplication>
#include <QLabel>
#include <QWidgetAction>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QFileInfo>
#include <QDateTime>
#include <QHash>
#include <QApplication>
#include <QEvent>
#include <memory>
#include <QMenu>
#include <QPointer>
#include <QVariantMap>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
QString bookIdentity(const void *volume) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    static const auto get = symbol<QString (*)(const void *)>("_ZNK7Content5getIdEv");
    return get ? get(volume) : QString();
#else
    static const auto get = symbol<QString (*)(const void *, const QString &)>("_ZNK7Content5getIdERK7QString");
    return get ? get(volume, QString()) : QString();
#endif
}
struct NativeBook {
    QString path;
    qint64 size;
    QDateTime modified;
    ReaderBookRoute route;
};
// Process-local only. A file replacement invalidates its remembered route.
QHash<QString, NativeBook> nativeBooks;
struct MenuCallbacks {
    std::function<bool()> ready, defaultReader;
};
MenuCallbacks &menuCallbacks() {
    // NickelHook can initialize the reader before this file's global constructors.
    // Construct these callbacks on first use so later initialization cannot erase them.
    static MenuCallbacks callbacks;
    return callbacks;
}
void addReaderChoice(void *controller, const void *volume, QMenu *menu);
struct Choice {
    QString identity;
    bool kobalt;
};
std::unique_ptr<Choice> pendingChoice;
// A new user action abandons any opening that never reached Nickel's reader
// handoff. The release which selected the action has already been delivered.
class ChoiceLifetime final : public QObject {
    bool eventFilter(QObject *, QEvent *event) override {
        switch (event->type()) {
        case QEvent::TouchBegin:
        case QEvent::MouseButtonPress:
        case QEvent::KeyPress:
            pendingChoice.reset();
            break;
        default:
            break;
        }
        return false;
    }

public:
    explicit ChoiceLifetime(QObject *parent) : QObject(parent) {}
};
}
bool previewBook(const void *volume) {
    static const auto preview = symbol<bool (*)(const void *)>("_ZNK6Volume9isPreviewEv");
    if (!volume)
        return false;
    if (preview)
        return preview(volume);
    // Both supported firmware families store preview accessibility as 6.
    static const auto values = symbol<QVariantMap (*)(const void *)>("_ZNK6Volume11getDbValuesEv");
    return values && values(volume).value("Accessibility").toInt() == 6;
}
void rememberNativeBook(const QString &identity, const QString &path, bool protectedBook) {
    const QFileInfo file(path);
    if (identity.isEmpty() || path.isEmpty() || !file.isFile())
        return;
    nativeBooks.insert(identity, {path, file.size(), file.lastModified(),
                                  protectedBook ? ReaderBookRoute::Protected : ReaderBookRoute::Remembered});
}
static ReaderBookRoute bookRoute(const void *volume, QString *identity, QString *path, bool inspectPublication) {
    using Route = ReaderBookRoute;
    if (previewBook(volume))
        return Route::Unsupported;
    static const auto values = symbol<QVariantMap (*)(const void *)>("_ZNK6Volume11getDbValuesEv");
    if (!volume || !values)
        return Route::Unsupported;
    const auto fields = values(volume);
    const auto mime = fields.value("MimeType").toString();
    // PDFs and other formats never show a Kobalt screen.
    if (mime != "application/epub+zip" && mime != "application/x-kobo-epub+zip")
        return Route::Unsupported;
    const auto id = bookIdentity(volume);
    if (identity)
        *identity = id;
    // Nickel imports a TXT file by writing its own EPUB beside it, whose
    // chapter paths CREngine cannot resolve. Nickel's reader opens it.
    if (id.endsWith(QLatin1String(".txt.epub"), Qt::CaseInsensitive))
        return Route::Unsupported;
    if (!fields.value("AdobeLocation").toString().isEmpty())
        return Route::Protected;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    static const auto adobe = symbol<bool (*)(const void *)>("_ZNK7Content10isAdobeDrmEv");
    static const auto lcp = symbol<bool (*)(const void *)>("_ZNK7Content9isLcpEpubEv");
    if (!adobe || !lcp)
        return Route::Unsupported;
    if (adobe(volume) || lcp(volume))
        return Route::Protected;
#endif
    auto filename = localReaderBookPath(id);
    const bool downloaded = filename.isEmpty();
    if (downloaded) {
        filename = downloadedReaderBookPath(id);
    }
    if (filename.isEmpty() || !QFileInfo(filename).isFile()) {
        return Route::Unsupported;
    }
    if (path)
        *path = filename;
    static const auto device = symbol<void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    static const auto dbName = symbol<const QString &(*)(const void *)>("_ZNK6Device9getDbNameEv");
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    static const auto connection = symbol<QSqlDatabase (*)(QString, bool)>("_ZN15DatabaseAdapter12dbConnectionE7QStringb");
#else
    static const auto connection = symbol<QSqlDatabase (*)(const QString &, bool)>("_ZN15DatabaseAdapter12dbConnectionERK7QStringb");
#endif
    const auto current = device ? device() : nullptr;
    const auto database = current && dbName && connection ? connection(dbName(current), false) : QSqlDatabase();
    const auto protection = bookProtection(database, id);
    if (protection == BookProtection::Unavailable)
        return Route::Unsupported;
    if (protection != BookProtection::None)
        return Route::Protected;
    const QFileInfo fileInfo(filename);
    const auto cached = nativeBooks.constFind(id);
    if (cached != nativeBooks.constEnd()) {
        if (cached->path == filename && cached->size == fileInfo.size() && cached->modified == fileInfo.lastModified())
            return cached->route;
        nativeBooks.remove(id);
    }
    if (!inspectPublication)
        return Route::Direct;
    QFile file(filename);
    Publication publication;
    QString error;
    if (!file.open(QIODevice::ReadOnly))
        return Route::Failed;
    if (!publication.inspect(file, &error)) {
        diagnostic("native-resource-failed",
                   {{"stage", "publication-inspection"}, {"error_code", diagnosticErrorCode(error)}}, kobalt::DiagnosticSeverity::Error);
        if (publication.protectedBook()) {
            rememberNativeBook(id, filename, true);
            return Route::Protected;
        }
        return Route::Failed;
    }
    return Route::Direct;
}
ReaderBookRoute readerBookRoute(const void *volume, QString *identity, QString *path) {
    return bookRoute(volume, identity, path, true);
}
ReaderBookRoute readerBookCandidateRoute(const void *volume, QString *identity, QString *path) {
    return bookRoute(volume, identity, path, false);
}
void configureBookMenu(std::function<bool()> available, std::function<bool()> preferred) {
    auto &callbacks = menuCallbacks();
    callbacks.ready = std::move(available);
    callbacks.defaultReader = std::move(preferred);
    static auto *lifetime = new ChoiceLifetime(qApp);
    qApp->installEventFilter(lifetime);
}
bool readerChoice(const void *volume, bool preferred) {
    auto choice = std::move(pendingChoice);
    if (!choice || choice->identity != bookIdentity(volume))
        return preferred;
    diagnostic("reader-choice-consumed", {{"kobalt", choice->kobalt}});
    return choice->kobalt;
}
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
void (*nativeBookMenu)(void *, void *, const void *, QMenu *, const QString &, const QString &, const void *);
extern "C" __attribute__((visibility("default"))) void kobalt_book_menu(void *self, void *controller,
                                                                        const void *volume, QMenu *menu,
                                                                        const QString &page, const QString &location,
                                                                        const void *tab) {
    nativeBookMenu(self, controller, volume, menu, page, location, tab);
    addReaderChoice(controller, volume, menu);
}
#else
void (*nativeBookMenu)(void *, void *, const void *, QMenu *, const QString &, const QString &, bool, const void *);
extern "C" __attribute__((visibility("default"))) void kobalt_book_menu(void *self, void *controller,
                                                                        const void *volume, QMenu *menu,
                                                                        const QString &page, const QString &location,
                                                                        bool flag, const void *tab) {
    nativeBookMenu(self, controller, volume, menu, page, location, flag, tab);
    addReaderChoice(controller, volume, menu);
}
#endif
namespace {
void addReaderChoice(void *controller, const void *volume, QMenu *menu) {
    const auto &callbacks = menuCallbacks();
    if (!menu || !callbacks.ready || !callbacks.ready() || !callbacks.defaultReader || !nativeBookMenu)
        return;
    const bool useKobalt = !callbacks.defaultReader();
    QString identity;
    // Only a Kobalt choice needs proof that Kobalt can read the publication.
    // Nickel must remain available for protected books and remembered fallbacks.
    const auto route = useKobalt ? readerBookRoute(volume, &identity) : readerBookCandidateRoute(volume, &identity);
    if (identity.isEmpty() || (useKobalt ? route != ReaderBookRoute::Direct : route == ReaderBookRoute::Unsupported))
        return;
    QPointer<QAction> original;
    const auto readNow = QCoreApplication::translate("ReadBookActionProxy", "Read now");
    for (auto *candidate : menu->actions()) {
        auto *widgetAction = qobject_cast<QWidgetAction *>(candidate);
        auto *widget = widgetAction ? widgetAction->defaultWidget() : nullptr;
        if (!widget)
            continue;
        for (auto *label : widget->findChildren<QLabel *>())
            if (label->text() == readNow) {
                original = candidate;
                break;
            }
        if (original)
            break;
    }
    if (!original || !original->isEnabled())
        return;
    static const auto item = symbol<QWidget *(*)(void *, QMenu *, const QString &, bool, bool, const QString &)>(
        "_ZN28AbstractNickelMenuController18createMenuTextItemEP5QMenuRK7QStringbbS4_");
    static const auto action = symbol<QAction *(*)(void *, QMenu *, QWidget *, bool, bool, bool)>(
        "_ZN22AbstractMenuController12createActionEP5QMenuP7QWidgetbbb");
    if (!item || !action)
        return;
    const Choice choice{identity, useKobalt};
    auto *widget =
        item(controller, menu,
             (choice.kobalt ? strings::get("native_ui.read_with_kobalt") : strings::get("native_ui.read_with_nickel")),
             false, false, "rrAlternativeReaderAction");
    if (!widget)
        return;
    widget->setObjectName("rrAlternativeReaderAction");
    auto *alternate = action(controller, menu, widget, true, true, true);
    if (!alternate) {
        widget->deleteLater();
        return;
    }
    auto actions = menu->actions();
    actions.removeAll(alternate);
    const int next = actions.indexOf(original) + 1;
    menu->removeAction(alternate);
    menu->insertAction(next < actions.size() ? actions[next] : nullptr, alternate);
    // The existing separator now follows the alternate action. Keep the
    // native separation between Read now and the inserted action as well.
    menu->insertSeparator(alternate);
    QObject::connect(alternate, &QAction::triggered, menu, [original, choice] {
        if (!original)
            return;
        // Nickel may finish authorization asynchronously. Consume this once at
        // its reader handoff, or discard it before another user action.
        pendingChoice = std::make_unique<Choice>(choice);
        original->trigger();
    });
}
}
}
