#include "support/qt_compat.h"
#include "support/strings.h"
#include "platform/nickel/common/native_dictionary.h"
#include "platform/nickel/common/dictionary_selection.h"
#include "support/diagnostics.h"
#include <QCoreApplication>
#include <QApplication>
#include <QPointer>
#include <QWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QMetaType>
#include <QSaveFile>
#include <QSharedPointer>
#include <QThread>
#include <QVariantMap>
#include <QHash>
#include <dlfcn.h>

namespace kobalt {
#ifndef KOBALT_READER_HARDWARE
void (*dictionaryTestNextReadingView)(void *, QWidget *) = nullptr;
void (*dictionaryTestNextWebView)(void *, QWidget *) = nullptr;
static quint64 readingViewConstructions = 0, webViewConstructions = 0;
extern "C" __attribute__((visibility("default"))) void kobalt_dictionary_reading_view(void *self, QWidget *parent) {
    ++readingViewConstructions;
    dictionaryTestNextReadingView(self, parent);
}
extern "C" __attribute__((visibility("default"))) void kobalt_dictionary_web_view(void *self, QWidget *parent) {
    ++webViewConstructions;
    dictionaryTestNextWebView(self, parent);
}
#endif
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
struct Dictionary {
    void *word;
    Dictionary(const Dictionary &) = delete;
    ~Dictionary() {
        symbol<void (*)(void *)>("_ZN10DictionaryD1Ev")(this);
    }
};
struct ContentSettings {
    // The native ABI requires a 120-byte ScObject value, not a QObject.
    alignas(8) char bytes[120];
    ContentSettings(const ContentSettings &) = delete;
    ~ContentSettings() {
        symbol<void (*)(void *)>("_ZN15ContentSettingsD1Ev")(this);
    }
};
struct Api {
    Volume (*volume)(const QString &,
                     const QString &) = symbol<decltype(volume)>("_ZN13VolumeManager7getByIdERK7QStringS2_");
    QVariantMap (*values)(const void *) = symbol<decltype(values)>("_ZNK6Volume11getDbValuesEv");
    const void *(*device)() = symbol<decltype(device)>("_ZN6Device16getCurrentDeviceEv");
    const QString &(*dbName)(const void *) = symbol<decltype(dbName)>("_ZNK6Device9getDbNameEv");
    ContentSettings (*settings)(const QString &, const int &, const QString &) =
        symbol<decltype(settings)>("_ZN21ContentSettingManager8retrieveERK7QStringRK11ContentTypeS2_");
    QList<Dictionary> (*all)(const void *) = symbol<decltype(all)>("_ZN21DictionaryFileManager6getAllERK6Device");
    const QString &(*suffix)(const Dictionary *) = symbol<decltype(suffix)>("_ZNK10Dictionary6suffixEv");
    const QString &(*path)(const Dictionary *) = symbol<decltype(path)>("_ZNK10Dictionary4pathEv");
    bool (*custom)(const Dictionary *) = symbol<decltype(custom)>("_ZNK10Dictionary8isCustomEv");
    // The setter accepts QVariant, but stores a QString and returns it by reference.
    void (*select)(ContentSettings *, const QVariant &) =
        symbol<decltype(select)>("_ZN15ContentSettings21setSelectedDictionaryERK8QVariant");
    Dictionary (*choose)(const Volume &, const ContentSettings &) =
        symbol<decltype(choose)>("_ZN16DictionaryParser19dictionaryForVolumeERK6VolumeRK15ContentSettings");
    QSharedPointer<QObject> (*parser)() = symbol<decltype(parser)>("_ZN16DictionaryParser14sharedInstanceEv");
    Dictionary (*active)(const QObject *) = symbol<decltype(active)>("_ZNK16DictionaryParser16activeDictionaryEv");
    void (*initialize)(QObject *, const Dictionary &) =
        symbol<decltype(initialize)>("_ZN16DictionaryParser10initializeERK10Dictionary");
    bool (*equal)(const Dictionary *, const Dictionary &) = symbol<decltype(equal)>("_ZNK10DictionaryeqERKS_");
    bool (*valid)(const Dictionary *) = symbol<decltype(valid)>("_ZNK10Dictionary7isValidEv");
    bool (*installed)(const Dictionary *) = symbol<decltype(installed)>("_ZNK10Dictionary9installedEv");
    QString (*language)(const Dictionary *) = symbol<decltype(language)>("_ZNK10Dictionary12leftLanguageEv");
    const QString &(*name)(const Dictionary *) = symbol<decltype(name)>("_ZNK10Dictionary4nameEv");
    QString (*source)(QObject *) = symbol<decltype(source)>("_ZN16DictionaryParser16dictionarySourceEv");
    void (*construct)(void *, const Dictionary &,
                      QObject *) = symbol<decltype(construct)>("_ZN18DictionaryProviderC1ERK10DictionaryP7QObject");
    QString (*find)(QObject *, const QString &) = symbol<decltype(find)>("_ZN18DictionaryProvider8findWordERK7QString");
    void (*definition)(QObject *, const QString &) =
        symbol<decltype(definition)>("_ZN18DictionaryProvider13getDefinitionERK7QString");
    bool ready() const {
        const auto type = [](const char *name) {
            return QMetaType(symbol<const QtPrivate::QMetaTypeInterface *>(name)).sizeOf();
        };
        return type("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI10DictionaryE8metaTypeE") == sizeof(Dictionary) &&
               type("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI18DictionaryProviderE8metaTypeE") == 28 &&
               type("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI6VolumeE8metaTypeE") == sizeof(Volume) && all && suffix &&
               path && custom && select && volume && values && device && dbName && settings && choose && parser &&
               active && initialize && equal && valid && installed && language && name && source && construct && find &&
               definition && symbol<void *>("_ZN6VolumeD1Ev") && symbol<void *>("_ZN10DictionaryD1Ev") &&
               symbol<void *>("_ZN15ContentSettingsD1Ev");
    }
};
const Api &api() {
    static const Api value;
    return value;
}
}
struct NativeDictionary::Data {
    QSharedPointer<QObject> parser;
    QHash<QString, QString> selected;
    bool usable = true;
};
NativeDictionary::NativeDictionary() : data(std::make_unique<Data>()) {}
NativeDictionary::~NativeDictionary() = default;
bool NativeDictionary::available() {
    return api().ready();
}
QVector<DictionaryChoice> NativeDictionary::choices() const {
    if (!available() || QThread::currentThread() != qApp->thread()) {
        return {};
    }
    return nickel::dictionaryChoices(api().all(api().device()), api());
}
bool NativeDictionary::select(const QString &book, const QString &id, QString *error) {
    if (!available() || QThread::currentThread() != qApp->thread() || pending) {
        *error = strings::get("error.dictionary_selection_is_unavailable");
        return false;
    }
    bool installed = false;
    for (const auto &choice : choices())
        installed |= choice.id == id;
    if (!installed) {
        *error = strings::get("error.that_dictionary_is_no_longer_installed");
        return false;
    }
    const auto &a = api();
    const auto database = a.dbName(a.device());
    const auto volume = a.volume(book, database);
    const auto values = a.values(&volume);
    if (values.value("ContentID").toString() != book) {
        *error = strings::get("error.the_book_is_no_longer_available");
        return false;
    }
    data->selected.insert(book, id);
    return true;
}
void NativeDictionary::definition(const QString &word, const QString &html, const QUrl &url) {
    if (!pending)
        return;
    pending->word = word;
    pending->html = html;
    pending->baseUrl = url;
    pending->received = true;
}
DictionaryResult NativeDictionary::lookup(const QString &book, const QString &word) {
    DictionaryResult result;
    if (!available() || !data->usable) {
        result.error = strings::get("error.dictionary_lookup_is_unavailable_on_this_firmware");
        return result;
    }
    if (QThread::currentThread() != qApp->thread() || thread() != qApp->thread() || pending) {
        result.error = strings::get("error.dictionary_lookup_is_busy");
        return result;
    }
    if (word.trimmed().isEmpty() || word.size() > 256) {
        result.error = strings::get("error.select_a_word_to_look_up");
        return result;
    }
    QElapsedTimer timer;
    timer.start();
    const auto &a = api();
    if (!data->parser)
        data->parser = a.parser();
    auto *parser = data->parser.data();
    if (!parser || parser->thread() != qApp->thread()) {
        result.error = strings::get("error.dictionary_lookup_has_an_unsupported_thread");
        return result;
    }
    const auto volume = a.volume(book, a.dbName(a.device()));
    const auto values = a.values(&volume);
    if (values.value("ContentID").toString() != book) {
        result.error = strings::get("error.the_book_is_no_longer_available");
        return result;
    }
    const int contentType = values.value("ContentType").toInt();
    auto settings = a.settings(book, contentType, a.dbName(a.device()));
    // Override only this lookup value; Nickel's saved content settings stay unchanged.
    if (data->selected.contains(book) && !data->selected.value(book).startsWith("custom:")) {
        a.select(&settings, QVariant(data->selected.value(book)));
    }
    const auto automatic = a.choose(volume, settings);
    auto id = data->selected.value(book);
    if (id.isEmpty() && a.valid(&automatic)) {
        id = a.suffix(&automatic);
    }
    const auto dictionaries = a.all(a.device());
    const auto *installed = nickel::installedDictionary(dictionaries, id, a);
    const auto &dictionary = installed ? *installed : automatic;
    if (!a.valid(&dictionary)) {
        result.error = strings::get("error.no_dictionary_is_available_for_this_book");
        return result;
    }
    result.language = a.language(&dictionary);
    result.dictionary = a.name(&dictionary);
    result.dictionaryId = nickel::dictionaryId(dictionary, a);
    if (!a.installed(&dictionary)) {
        result.error = strings::get("error.the_selected_dictionary_is_not_installed");
        return result;
    }
    const auto previous = a.active(parser);
    // Provider initialization changes the shared parser. Keep the whole lookup
    // synchronous and restore it before returning to Nickel's event loop.
    struct Restore {
        QObject *parser;
        const Dictionary &previous;
        DictionaryResult &result;
        bool done = false;
        void run() noexcept {
            if (done)
                return;
            done = true;
            try {
                api().initialize(parser, previous);
                const auto current = api().active(parser);
                result.restored = api().equal(&current, previous);
            } catch (...) {
                result.restored = false;
            }
        }
        ~Restore() {
            run();
        }
    } restore{parser, previous, result};
    try {
        void *storage = ::operator new(28);
        try {
            a.construct(storage, dictionary, nullptr);
        } catch (...) {
            ::operator delete(storage);
            throw;
        }
        std::unique_ptr<QObject> provider(static_cast<QObject *>(storage));
        if (!connect(provider.get(), SIGNAL(gotDefinition(QString, QString, QUrl)), this,
                     SLOT(definition(QString, QString, QUrl)), Qt::DirectConnection)) {
            result.error = strings::get("error.dictionary_lookup_could_not_connect");
        } else {
            // DictionaryView::showWord skips NFKC for Japanese, then trims.
            auto candidate = result.language == "ja" ? word : word.normalized(QString::NormalizationForm_KC);
            candidate = a.find(provider.get(), candidate.trimmed());
            pending = &result;
            a.definition(provider.get(), candidate);
            pending = nullptr;
            result.source = a.source(parser);
            if (!result.received)
                result.error = strings::get("error.dictionary_lookup_did_not_return_a_definition");
            else if (result.html.isEmpty())
                result.error = strings::get("error.no_definition_found");
        }
    } catch (...) {
        pending = nullptr;
        result.error = strings::get("error.the_dictionary_could_not_be_loaded");
    }
    restore.run();
    if (!result.restored) {
        data->usable = false;
        result.error = strings::get("error.dictionary_state_could_not_be_restored");
    }
    result.elapsedMs = timer.elapsed();
    diagnostic("dictionary-lookup", {{"elapsed_ms", double(result.elapsedMs)},
                                     {"received", result.received},
                                     {"restored", result.restored},
                                     {"failed", !result.error.isEmpty()}});
    return result;
}
#ifndef KOBALT_READER_HARDWARE
void NativeDictionary::testCommand() {
    QFile input("/instance/dictionary-test.json");
    if (!input.open(QIODevice::ReadOnly))
        return;
    if (input.size() > 4096) {
        input.close();
        input.remove();
        return;
    }
    const auto command = QJsonDocument::fromJson(input.readAll()).object();
    input.close();
    input.remove();
    QJsonObject output = kobalt::qt::object({{"id", command.value("id")}, {"available", available()}});
    // Never expose a general book/content dump through the virtual test hook.
    const auto book = command.value("book").toString();
    if (book == "file:///mnt/onboard/dictionary-selection.kepub.epub" ||
        book == "file:///mnt/onboard/dictionary-selection-fr.kepub.epub") {
        static QPointer<NativeDictionary> adapter;
        if (!adapter) {
            adapter = new NativeDictionary;
            adapter->setParent(qApp);
        }
        QJsonArray options;
        for (const auto &choice : adapter->choices())
            options.append(kobalt::qt::object({{"id", choice.id}, {"name", choice.name}}));
        output["choices"] = options;
        if (command.contains("select")) {
            QString error;
            output["selected"] = adapter->select(book, command.value("select").toString(), &error);
            output["selection_error"] = error;
        }
        const auto views = readingViewConstructions, webViews = webViewConstructions;
        const auto result = adapter->lookup(book, command.value("word").toString());
        output["word"] = result.word;
        output["html"] = result.html;
        output["source"] = result.source;
        output["dictionary"] = result.dictionary;
        output["dictionary_id"] = result.dictionaryId;
        output["language"] = result.language;
        output["error"] = result.error;
        output["received"] = result.received;
        output["restored"] = result.restored;
        output["elapsed_ms"] = double(result.elapsedMs);
        output["reading_view_constructions"] = double(readingViewConstructions - views);
        output["webengine_view_constructions"] = double(webViewConstructions - webViews);
        output["construction_counters_available"] = dictionaryTestNextReadingView && dictionaryTestNextWebView;
        int renderers = 0;
        for (auto *widget : QApplication::allWidgets())
            renderers += widget->inherits("PageImageReader");
        output["live_native_renderers"] = renderers;
    }
    QSaveFile file("/instance/artifacts/dictionary-test.json");
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(output).toJson());
        file.commit();
    }
}
#endif
}
