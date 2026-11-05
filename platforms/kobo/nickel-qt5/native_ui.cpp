#include "support/qt_compat.h"
#include "platforms/kobo/common/native_ui.h"
#include "support/strings.h"
#include "support/diagnostics.h"
#include "platforms/kobo/common/publication_source.h"
#include "native_labels.h"
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
#include <QCryptographicHash>
#include <QFont>
#include <QGuiApplication>
#include <QScreen>
#include <QLabel>
#include <QPointer>
#include <QEvent>
#include <QCheckBox>
#include <QBoxLayout>
#include <QFrame>
#include <NickelHook.h>
#include <QKeyEvent>
#include <QLocale>
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
}
QStringList nativeReaderFontFamilies() {
    if (!qApp || QThread::currentThread() != qApp->thread())
        return {};
    static const auto shared = symbol<void *(*)()>("_ZN11FontManager14sharedInstanceEv");
    static const auto bundled = symbol<QStringList (*)(void *)>("_ZN11FontManager19bundledFontFamiliesEv");
    static const auto setup =
        symbol<void (*)(QStringList &)>("_ZN14N3FontTypeUtil19commonDropdownSetupER11QStringList");
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
    static const auto bw = symbol<bool (*)(const void *)>("_ZNK6Device11isSpaKoboBWEv");
    const auto *device = current ? current() : nullptr;
    return device && bw && bw(device);
}
int nativePageButtonDirection(QKeyEvent *event) {
    // Firmware 4.46 exports no button mapping. In its stock reader the lower
    // button, F24, turns forward unless the Reading setting inverts the pair.
    const int key = event->key();
    if (key == Qt::Key_Left || key == Qt::Key_Up)
        return -1;
    if (key == Qt::Key_Right || key == Qt::Key_Down)
        return 1;
    if (key != Qt::Key_F23 && key != Qt::Key_F24)
        return 0;
    QSettings settings("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf", QSettings::IniFormat);
    bool inverted = settings.value("Reading/invertPageTurnButtons", false).toBool();
    // The buttons sit on one edge of the device. Turned upside down, the lower
    // button is the physically upper one, so the pair swaps.
    const auto *screen = QGuiApplication::primaryScreen();
    if (screen && (screen->orientation() == Qt::InvertedPortraitOrientation ||
                   screen->orientation() == Qt::InvertedLandscapeOrientation))
        inverted = !inverted;
    return (key == Qt::Key_F24) != inverted ? 1 : -1;
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
    // N3BatteryStatusLabel::updateBatteryLevelNow uses 0x84 on 4.38.23697
    // and 0x90 on 4.46.23836. Call only an exported hardware implementation we
    // know, not HardwareInterface's base implementation, which returns 40.
    for (const auto offset : {0x84, 0x90}) {
        const auto level =
            reinterpret_cast<int (*)(const void *)>((*static_cast<void ***>(device))[offset / sizeof(void *)]);
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
    }
    return {};
}
QString nativeReaderClock() {
    return nativeReaderTime(QTime::currentTime());
}
QString nativeReaderTime(QTime time) {
    const QLocale locale;
    auto format = locale.timeFormat(QLocale::ShortFormat);
    // These native methods return the native section name and locale default.
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
namespace {
// Returns the index of the item in a box layout that holds the widget, at any depth.
int itemIndex(QLayout *layout, const QWidget *widget) {
    for (int i = 0; layout && i < layout->count(); ++i) {
        auto *item = layout->itemAt(i);
        if (item->widget() == widget || (item->widget() && item->widget()->isAncestorOf(widget)))
            return i;
        if (item->layout() && itemIndex(item->layout(), widget) >= 0)
            return i;
    }
    return -1;
}
}
void installNativeReaderSetting(QWidget *view, std::function<bool()> enabled, std::function<bool(bool)> save) {
    if (view->findChild<QWidget *>("rrAlternativeReader"))
        return;
    // Firmware 4 builds each Reading Settings row from a label and a
    // TouchCheckBox. It has no SettingItemWithCheckBox, and its label classes
    // export no constructor. TouchCheckBox requires 88 bytes on 4.46.23836.
    auto *section = view->findChild<QWidget *>("newReadingProgressContainer");
    auto *rows = section ? qobject_cast<QBoxLayout *>(section->layout()) : nullptr;
    auto *referenceTitle = view->findChild<QLabel *>("newReadingProgressTitle");
    auto *referenceLabel = view->findChild<QLabel *>("bookProgressBar");
    auto *referenceCheck = view->findChild<QCheckBox *>("showBookProgressBar");
    const auto construct = symbol<void (*)(void *, QWidget *)>("_ZN13TouchCheckBoxC1EP7QWidget");
    diagnostic("native-settings-controls", {{"rows", bool(rows)},
                                            {"title", bool(referenceTitle)},
                                            {"label", bool(referenceLabel)},
                                            {"checkbox", bool(referenceCheck)},
                                            {"constructor", bool(construct)},
                                            {"archive", native::publicationSource().available()}});
    if (!rows || !referenceTitle || !referenceLabel || !referenceCheck || !construct ||
        !native::publicationSource().available()) {
        nh_log("reader: native settings controls unavailable");
        return;
    }
    void *storage = ::operator new(88);
    construct(storage, section);
    auto *check = qobject_cast<QCheckBox *>(static_cast<QObject *>(storage));
    if (!check) {
        delete static_cast<QObject *>(storage);
        nh_log("reader: native settings controls unavailable");
        return;
    }
    check->setObjectName("rrAlternativeReaderEnabled");
    check->setText(referenceCheck->text());
    check->setFont(referenceCheck->font());
    check->setSizePolicy(referenceCheck->sizePolicy());
    check->setMinimumSize(referenceCheck->minimumSize());
    check->setMaximumSize(referenceCheck->maximumSize());
    check->setChecked(enabled());
    auto *heading = new MetaDataHeaderLabel(section);
    heading->setObjectName("rrAlternativeReaderHeading");
    heading->setText(strings::get("app.name").toUpper());
    heading->setSizePolicy(referenceTitle->sizePolicy());
    heading->setAlignment(referenceTitle->alignment());
    auto *label = new RegularLabelVPadded(section);
    label->setObjectName("rrAlternativeReaderLabel");
    label->setText(strings::get("native_ui.open_books_in_kobalt"));
    label->setSizePolicy(referenceLabel->sizePolicy());
    label->setAlignment(referenceLabel->alignment());
    auto *row = new QWidget(section);
    row->setObjectName("rrAlternativeReader");
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(0);
    rowLayout->addWidget(label, 1);
    rowLayout->addWidget(check, 0, Qt::AlignVCenter);
    auto *error = new QLabel(strings::get("native_ui.could_not_save_reader_preference"), section);
    error->setObjectName("rrAlternativeReaderError");
    error->setFont(referenceLabel->font());
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
    // Nickel sets these fonts through rules for its own widget names. They
    // resolve when the page is polished, so copy them when it is first shown.
    class Fonts : public QObject {
    public:
        Fonts(QObject *parent, std::function<void()> apply) : QObject(parent), apply(std::move(apply)) {}
        bool eventFilter(QObject *, QEvent *event) override {
            if (event->type() == QEvent::Show || event->type() == QEvent::Polish)
                apply();
            return false;
        }

    private:
        std::function<void()> apply;
    };
    const QPointer<QLabel> title = referenceTitle, text = referenceLabel;
    const QPointer<QCheckBox> box = referenceCheck;
    view->installEventFilter(new Fonts(row, [=] {
        const kobalt::qt::SignalBlocker blocked(check);
        check->setChecked(enabled());
        error->hide();
        if (!title || !text || !box)
            return;
        heading->setFont(title->font());
        label->setFont(text->font());
        check->setFont(box->font());
        label->setMinimumHeight(text->height());
        heading->setFixedHeight(title->height());
        // Nickel's stylesheet sets these when it polishes its own labels. The
        // class rules give Kobalt's labels the same padding; add padding only
        // where a rule for a widget name gave Nickel's label more.
        heading->setIndent(title->indent());
        label->setIndent(text->indent());
        heading->setAlignment(title->alignment());
        label->setAlignment(text->alignment());
        const auto indent = [](QLabel *own, const QLabel *reference) {
            const int wanted = reference->contentsRect().left();
            if (own->styleSheet().isEmpty() && own->contentsRect().left() != wanted)
                own->setStyleSheet(QString("padding-left:%1px;").arg(wanted));
        };
        indent(heading, title);
        indent(label, text);
    }));
    // Place the Kobalt group above Nickel's first group, as on firmware 5.
    const int first = qMax(0, itemIndex(rows, referenceTitle));
    rows->insertWidget(first, heading);
    rows->insertWidget(first + 1, row);
    rows->insertWidget(first + 2, error);
    diagnostic("native-settings-inserted");
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
    void (*open)(void *, bool, const QUrl &, const QString &) =
        symbol<decltype(open)>("_ZN22BrowserWorkflowManager11openBrowserEbRK4QUrlRK7QString");
    size_t bytes() const {
        if (!construct || !open)
            return 0;
        // The inspected Qt5 constructor belongs to the 20-byte QObject layout.
        const auto *code = reinterpret_cast<const char *>(reinterpret_cast<uintptr_t>(construct) & ~uintptr_t(1));
        Dl_info info{};
        void *extra = nullptr;
        if (!dladdr1(reinterpret_cast<void *>(construct), &info, &extra, RTLD_DL_SYMENT) || !extra ||
            static_cast<const ElfW(Sym) *>(extra)->st_size != 68 || !browserReadable(code, 68, PF_R | PF_X))
            return 0;
        return QCryptographicHash::hash(QByteArray(code, 68), QCryptographicHash::Sha256).toHex() ==
                       "835e8969b06415b443cf38e0b31a233a99b0dc2df61d3f5aa1beef3b0db89b85"
                   ? 20
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
    browserApi().open(request->manager, true, request->url, request->css);
    request->handoff.check();
    return true;
}
}
