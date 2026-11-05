#include "support/qt_compat.h"
#include "support/strings.h"
#include "platforms/kobo/common/native_ui.h"
#include <QApplication>
#include <QUrl>
#include <QWidget>
#include "platforms/kobo/common/native_lifecycle.h"
#include "platforms/kobo/common/browser_handoff.h"
#include <link.h>
#include <QPointer>
#include <QThread>
#include <new>
#include <cstddef>
#include <cstdint>
#include <QFont>
#include <QFrame>
#include <NickelHook.h>
#include <QBoxLayout>
#include <QCheckBox>
#include <QLabel>
#include <QKeyEvent>
#include <QEvent>
#include <utility>
#include <QLocale>
#include <QMetaType>
#include <QSettings>
#include <QTime>
#include <QCoreApplication>
#include <QThread>
#include <QVariant>
#include <dlfcn.h>

namespace kobalt {
namespace {
template <class T> T symbol(const char *name) {
    return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}
QWidget *nativeWidget(const char *typeSymbol, QWidget *parent) {
    const auto *interface = symbol<const QtPrivate::QMetaTypeInterface *>(typeSymbol);
    if (!interface)
        return nullptr;
    const QMetaType type(interface);
    auto *widget = static_cast<QWidget *>(type.create());
    if (widget)
        widget->setParent(parent);
    return widget;
}

}
QStringList nativeReaderFontFamilies() {
    if (!qApp || QThread::currentThread() != qApp->thread())
        return {};
    static const auto shared = symbol<void *(*)()>("_ZN11FontManager14sharedInstanceEv");
    static const auto bundled = symbol<QStringList (*)(void *)>("_ZN11FontManager19bundledFontFamiliesEv");
    static const auto setup =
        symbol<void (*)(QStringList &)>("_ZN14N3FontTypeUtil19commonDropdownSetupER5QListI7QStringE");
    if (!shared || !bundled || !setup)
        return {};
    auto *manager = shared();
    if (!manager)
        return {};
    // Fontickel supplies the active mode's bundled families. The native dropdown
    // then combines sources and applies its locale ordering. Keep only built-ins.
    const auto installed = bundled(manager);
    QStringList offered;
    setup(offered);
    QStringList result;
    for (const auto &family : offered)
        if (installed.contains(family) && !result.contains(family))
            result.append(family);
    return result;
}
bool nativeReaderHasPageButtons() {
    static const auto current = symbol<const void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    static const auto buttons = symbol<bool (*)(const void *)>("_ZNK6Device16hasPagingButtonsEv");
    const auto *device = current ? current() : nullptr;
    return device && buttons && buttons(device);
}
bool nativeReaderReverseLandscapeButtons() {
    static const auto current = symbol<const void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    static const auto spa = symbol<bool (*)(const void *)>("_ZNK6Device9isSpaKoboEv");
    static const auto bw = symbol<bool (*)(const void *)>("_ZNK6Device11isSpaKoboBWEv");
    const auto *device = current ? current() : nullptr;
    // Confirmed on the Clara Colour: Landscape puts its bottom bezel on the left.
    return device && ((spa && spa(device)) || (bw && bw(device)));
}
int nativePageButtonDirection(QKeyEvent *event) {
    static const auto map = symbol<int (*)(QKeyEvent *)>("_ZN19TactileButtonFilter15getKeyForButtonEP9QKeyEvent");
    static const auto forward = symbol<bool (*)(int, bool)>("_ZN19TactileButtonFilter18isForwardDirectionEib");
    if (!map || !forward)
        return 0;
    const int key = event->key();
    if (key != Qt::Key_F23 && key != Qt::Key_F24 && key != Qt::Key_Up && key != Qt::Key_Down && key != Qt::Key_Left &&
        key != Qt::Key_Right)
        return 0;
    const int mapped = key == Qt::Key_F23 || key == Qt::Key_F24 ? map(event) : key;
    if (mapped != Qt::Key_Up && mapped != Qt::Key_Down && mapped != Qt::Key_Left && mapped != Qt::Key_Right)
        return 0;
    // Match Kobalt's existing left-to-right page navigation. Nickel owns the
    // physical-button orientation and invertPageTurnButtons mapping.
    return forward(mapped, true) ? 1 : -1;
}
bool nativeReaderSleepScreenAvailable() {
    const auto currentDevice = symbol<const void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    const auto tolino = symbol<bool (*)(const void *)>("_ZNK6Device8isTolinoEv");
    const auto *device = currentDevice ? currentDevice() : nullptr;
    return device && tolino && !tolino(device);
}
QString nativeReaderCoverPath(const void *volume) {
    const auto currentDevice = symbol<const void *(*)()>("_ZN6Device16getCurrentDeviceEv");
    const auto imageId = symbol<QString (*)(const void *)>("_ZNK7Content10getImageIdEv");
    const auto defaultCover = symbol<bool (*)(const QString &)>("_ZN5Image19isDefaultCoverImageERK7QString");
    const auto fileName = symbol<QString (*)(const void *, const void *, const QString &)>(
        "_ZN5Image11getFileNameERK6DeviceRK6VolumeRK7QString");
    if (!nativeReaderSleepScreenAvailable() || !volume || !currentDevice || !imageId || !defaultCover || !fileName) {
        return {};
    }
    const auto *device = currentDevice();
    const auto id = imageId(volume);
    if (!device || id.isEmpty() || defaultCover(id)) {
        return {};
    }
    // Nickel resolves internal and removable-storage caches for this volume.
    // N3_FULL is its full-screen image type; do not substitute a library thumbnail.
    return fileName(device, volume, QStringLiteral("N3_FULL"));
}
NativeBatteryStatus nativeReaderBattery() {
    static const auto hardware = symbol<void *(*)()>("_ZN15HardwareFactory14sharedInstanceEv");
    static const auto charging = symbol<bool (*)(void *)>("_ZN17HardwareInterface10isChargingEv");
    if (!hardware || !charging)
        return {};
    void *device = hardware();
    if (!device)
        return {};
    // N3BatteryStatusLabel::updateBatteryLevelNow dispatches this virtual slot
    // in firmware 5.18. Call only an exported hardware implementation we know,
    // not HardwareInterface's base implementation, which always returns 40.
    const auto level = reinterpret_cast<int (*)(const void *)>((*static_cast<void ***>(device))[0x9c / sizeof(void *)]);
    struct Implementation {
        const char *level, *charge;
    };
    for (const auto &implementation :
         {Implementation{"_ZNK3MTK15getBatteryLevelEv", "_ZN3MTK13chargingStateEv"},
          Implementation{"_ZNK6iMX50815getBatteryLevelEv", "_ZN6iMX50813chargingStateEv"},
          Implementation{"_ZNK9AllWinner15getBatteryLevelEv", "_ZN9AllWinner13chargingStateEv"}}) {
        if (level && level == symbol<int (*)(const void *)>(implementation.level)) {
            const int percent = level(device);
            // Nickel uses 0 for battery power, 1 for charging and 2 for charged.
            const auto chargeState = symbol<int (*)(void *)>(implementation.charge);
            const int state = chargeState ? chargeState(device) : (charging(device) ? 1 : 0);
            return {percent >= 0 && percent <= 100 ? percent : -1, state == 1 || state == 2, state == 2};
        }
    }
    return {};
}
QString nativeReaderClock() {
    return nativeReaderTime(QTime::currentTime());
}
QString nativeReaderTime(QTime time) {
    const QLocale locale;
    auto format = locale.timeFormat(QLocale::ShortFormat);
    // These 5.18 methods return the native section name and locale default.
    // sectionName does not access its instance. Read the same file as Settings.
    static const auto section =
        symbol<const QLatin1String &(*)(const void *)>("_ZNK19ApplicationSettings11sectionNameEv");
    static const auto twelveHour = symbol<bool (*)()>("_ZN8TimeUtil13is12HourClockEv");
    QSettings settings("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf", QSettings::IniFormat);
    const bool use24 = section && twelveHour
                           ? settings.value(QString(section(nullptr)) + "/is24HourClock", !twelveHour()).toBool()
                           : !format.contains("AP", Qt::CaseInsensitive);
    if (use24) {
        format.remove(" AP", Qt::CaseInsensitive).remove("AP", Qt::CaseInsensitive);
    } else if (!format.contains("AP", Qt::CaseInsensitive)) {
        format.replace("HH", "h").replace("H", "h");
        format += " AP";
    }
    auto text = locale.toString(time, format.trimmed());
    if (!use24) {
        for (const auto &period : {locale.amText(), locale.pmText()}) {
            if (!period.isEmpty() && text.endsWith(period, Qt::CaseInsensitive)) {
                const int start = text.size() - period.size();
                text = text.left(start).trimmed() + ' ' + text.mid(start);
                break;
            }
        }
    }
    return text;
}
void installNativeReaderSetting(QWidget *view, std::function<bool()> enabled, std::function<bool(bool)> save) {
    auto *section = view->findChild<QWidget *>("newReadingProgressContainer");
    auto *rows = section ? qobject_cast<QBoxLayout *>(section->layout()) : nullptr;
    auto *reference = view->findChild<QWidget *>("showBookProgressBar");
    auto *referenceCheck = reference ? reference->findChild<QCheckBox *>() : nullptr;
    const auto setLabel = symbol<void (*)(void *, const QString &)>("_ZN15SettingItemBase8setLabelERK7QString");
    const auto setText = symbol<void (*)(void *, const QString &)>("_ZN23SettingItemWithCheckBox7setTextERK7QString");
    const auto setChecked = symbol<void (*)(void *, bool)>("_ZN23SettingItemWithCheckBox10setCheckedEb");
    if (!rows || !referenceCheck || !setLabel || !setText || !setChecked)
        return;
    auto *heading = qobject_cast<QLabel *>(
        nativeWidget("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI19MetaDataHeaderLabelE8metaTypeE", section));
    auto *row = nativeWidget("_ZN9QtPrivate25QMetaTypeInterfaceWrapperI23SettingItemWithCheckBoxE8metaTypeE", section);
    auto *check = row ? row->findChild<QCheckBox *>() : nullptr;
    if (!heading || !check) {
        delete heading;
        delete row;
        nh_log("reader: native settings controls unavailable");
        return;
    }
    heading->setObjectName("rrAlternativeReaderHeading");
    heading->setText(strings::get("app.name"));
    row->setObjectName("rrAlternativeReader");
    check->setObjectName("rrAlternativeReaderEnabled");
    for (const auto &name : reference->dynamicPropertyNames())
        row->setProperty(name.constData(), reference->property(name.constData()));
    row->setSizePolicy(reference->sizePolicy());
    row->setMinimumSize(reference->minimumSize());
    row->setMaximumSize(reference->maximumSize());
    row->setContentsMargins(reference->contentsMargins());
    setLabel(row, strings::get("native_ui.open_books_in_kobalt"));
    setText(row, referenceCheck->text());
    setChecked(row, enabled());
    auto *error = new QLabel(strings::get("native_ui.could_not_save_reader_preference"), section);
    error->setObjectName("rrAlternativeReaderError");
    error->setWordWrap(true);
    error->hide();
    QObject::connect(check, &QCheckBox::toggled, row, [check, error, enabled, save](bool on) {
        if (!save(on)) {
            const kobalt::qt::SignalBlocker blocked(check);
            check->setChecked(enabled());
            error->show();
        } else
            error->hide();
    });
    class SelectionState final : public QObject {
    public:
        SelectionState(QObject *parent, std::function<void()> refresh) : QObject(parent), refresh(std::move(refresh)) {}
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Show)
                refresh();
            return false;
        }

    private:
        std::function<void()> refresh;
    };
    view->installEventFilter(new SelectionState(row, [=] {
        const kobalt::qt::SignalBlocker blocked(check);
        check->setChecked(enabled());
        error->hide();
    }));
    auto *divider = new QFrame(section);
    divider->setFixedHeight(1);
    divider->setStyleSheet("background:#999;");
    rows->insertWidget(0, heading);
    rows->insertWidget(1, row);
    rows->insertWidget(2, error);
    rows->insertWidget(3, divider);
    nh_log("reader: inserted Kobalt setting");
}
QFont nativeReaderUiFont() {
    // Nickel's clock and navigation controls use this firmware font alias.
    // QApplication's default and an unstyled native label use other families.
    QFont font = QApplication::font();
    font.setFamily("DefaultSansSerif");
    return font;
}
}

namespace kobalt {
namespace {
bool browserReadable(const void *address, size_t bytes, unsigned flags) {
    struct Range {
        uintptr_t address;
        size_t bytes;
        unsigned flags;
        bool valid;
    };
    Range range{reinterpret_cast<uintptr_t>(address), bytes, flags, false};
    if (!address || !bytes)
        return false;
    dl_iterate_phdr(
        [](dl_phdr_info *info, size_t, void *opaque) {
            auto &range = *static_cast<Range *>(opaque);
            for (unsigned i = 0; i < info->dlpi_phnum; ++i) {
                const auto &segment = info->dlpi_phdr[i];
                if (segment.p_type != PT_LOAD || (segment.p_flags & range.flags) != range.flags)
                    continue;
                const auto start = info->dlpi_addr + segment.p_vaddr;
                if (range.address >= start && range.address - start < segment.p_memsz &&
                    range.bytes <= segment.p_memsz - (range.address - start)) {
                    range.valid = true;
                    return 1;
                }
            }
            return 0;
        },
        &range);
    return range.valid;
}
struct BrowserApi {
    void (*construct)(void *, QObject *) = symbol<decltype(construct)>("_ZN22BrowserWorkflowManagerC1EP7QObject");
    void (*open)(void *, bool, const QUrl &, const QString &,
                 bool) = symbol<decltype(open)>("_ZN22BrowserWorkflowManager11openBrowserEbRK4QUrlRK7QStringb");
    const QtPrivate::QMetaTypeInterface *interface = symbol<const QtPrivate::QMetaTypeInterface *>(
        "_ZN9QtPrivate25QMetaTypeInterfaceWrapperI22BrowserWorkflowManagerE8metaTypeE");
    size_t bytes() const {
        if (!browserReadable(interface, sizeof(*interface), PF_R) ||
            interface->revision != QMetaType::fromType<QObject *>().iface()->revision)
            return 0;
        const QMetaType type(interface);
        return construct && open && type.sizeOf() >= qsizetype(sizeof(QObject)) && type.sizeOf() <= 4096 &&
                       type.alignOf() <= qsizetype(alignof(std::max_align_t))
                   ? size_t(type.sizeOf())
                   : 0;
    }
};
const BrowserApi &browserApi() {
    static const BrowserApi value;
    return value;
}
// Nickel can retain the URL and CSS while connecting to Wi-Fi. Keep both with
// the workflow manager until application shutdown, rather than on the stack.
struct BrowserRequest : QObject {
    explicit BrowserRequest(const QUrl &value) : QObject(qApp), handoff(this), url(value) {}
    BrowserHandoff handoff;
    QUrl url;
    QString css;
    QObject *manager = nullptr;
};
}
bool nativeReaderBrowserAvailable() {
    return qApp && QThread::currentThread() == qApp->thread() && browserApi().bytes();
}
bool nativeReaderOpenBrowser(const QUrl &url, QWidget *reader) {
    if (!nativeReaderBrowserAvailable() || !url.isValid() || url.host().isEmpty() ||
        (url.scheme() != "http" && url.scheme() != "https"))
        return false;
    static QPointer<BrowserRequest> request;
    if (!request) {
        request = new BrowserRequest(url);
        void *memory = ::operator new(browserApi().bytes(), std::nothrow);
        if (!memory) {
            delete request.data();
            return false;
        }
        try {
            browserApi().construct(memory, request);
        } catch (...) {
            ::operator delete(memory);
            delete request.data();
            return false;
        }
        request->manager = static_cast<QObject *>(memory);
    }
    request->url = url;
    request->handoff.begin(reader, [] {
        return !NativeLifecycle::suspended();
    });
    browserApi().open(request->manager, true, request->url, request->css, false);
    request->handoff.check();
    return true;
}
}
