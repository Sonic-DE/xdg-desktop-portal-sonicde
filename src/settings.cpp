/*
 * SPDX-FileCopyrightText: 2018-2019 Red Hat Inc
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * SPDX-FileCopyrightText: 2018-2019 Jan Grulich <jgrulich@redhat.com>
 * SPDX-FileCopyrightText: 2023 Harald Sitter <sitter@kde.org>
 */

#include "settings.h"
#include "settings_debug.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QFile>
#include <QPalette>
#include <QStandardPaths>

#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include <KConfigGroup>
#include <KConfigWatcher>

#include "desktopportal.h"
using namespace Qt::Literals::StringLiterals;

namespace {
struct XInputDeviceState {
    bool touchAvailable = false;
    bool tabletModeAvailable = false;
    bool tabletModeEnabled = false;
};

static xcb_atom_t internAtom(xcb_connection_t* connection, const QByteArray& name)
{
    auto* reply = xcb_intern_atom_reply(connection, xcb_intern_atom(connection, false, name.size(), name.constData()), nullptr);
    if (!reply)
        return XCB_ATOM_NONE;
    const xcb_atom_t atom = reply->atom;
    free(reply);
    return atom;
}

static XInputDeviceState queryXInputDeviceState()
{
    XInputDeviceState state;
    xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection)
            xcb_disconnect(connection);
        return state;
    }
    auto* reply = xcb_input_xi_query_device_reply(connection,
        xcb_input_xi_query_device(connection, XCB_INPUT_DEVICE_ALL), nullptr);
    if (!reply) {
        xcb_disconnect(connection);
        return state;
    }
    const xcb_atom_t tabletModeAtom = internAtom(connection, QByteArrayLiteral("libinput Tablet Mode Switch"));
    for (auto it = xcb_input_xi_query_device_infos_iterator(reply); it.rem; xcb_input_xi_device_info_next(&it)) {
        const auto* device = it.data;
        for (auto classIt = xcb_input_xi_device_info_classes_iterator(device); classIt.rem; xcb_input_device_class_next(&classIt)) {
            if (classIt.data->type == XCB_INPUT_DEVICE_CLASS_TYPE_TOUCH)
                state.touchAvailable = true;
        }
        if (tabletModeAtom == XCB_ATOM_NONE)
            continue;
        auto* property = xcb_input_xi_get_property_reply(connection,
            xcb_input_xi_get_property(connection, device->deviceid, false, tabletModeAtom,
                XCB_ATOM_ANY, 0, 1),
            nullptr);
        if (!property || property->num_items == 0) {
            free(property);
            continue;
        }
        state.tabletModeAvailable = true;
        const auto* value = xcb_input_xi_get_property_items(property);
        if (property->format == 8)
            state.tabletModeEnabled = *reinterpret_cast<const uint8_t*>(value) != 0;
        else if (property->format == 32)
            state.tabletModeEnabled = *reinterpret_cast<const uint32_t*>(value) != 0;
        free(property);
    }
    free(reply);
    xcb_disconnect(connection);

    QFile chassis(QStringLiteral("/sys/class/dmi/id/chassis_type"));
    if (chassis.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const int type = QString::fromLatin1(chassis.readAll()).trimmed().toInt(&ok);
        if (ok && (type == 30 || type == 31 || type == 32))
            state.tabletModeAvailable = true;
    }
    return state;
}

struct VirtualKeyboardState {
    bool available = false;
    bool visible = false;
};

static VirtualKeyboardState queryVirtualKeyboardState()
{
    VirtualKeyboardState state;
    static const QStringList executableNames{u"maliit-keyboard"_s, u"onboard"_s, u"florence"_s};
    state.available = std::ranges::any_of(executableNames, [](const QString& name) {
        return !QStandardPaths::findExecutable(name).isEmpty();
    });

    int screenNumber = 0;
    xcb_connection_t* connection = xcb_connect(nullptr, &screenNumber);
    if (!connection || xcb_connection_has_error(connection)) {
        if (connection)
            xcb_disconnect(connection);
        return state;
    }
    auto screenIt = xcb_setup_roots_iterator(xcb_get_setup(connection));
    for (int i = 0; i < screenNumber; ++i)
        xcb_screen_next(&screenIt);
    if (!screenIt.data) {
        xcb_disconnect(connection);
        return state;
    }
    auto* tree = xcb_query_tree_reply(connection, xcb_query_tree(connection, screenIt.data->root), nullptr);
    if (!tree) {
        xcb_disconnect(connection);
        return state;
    }
    const auto* children = xcb_query_tree_children(tree);
    for (int i = 0; i < xcb_query_tree_children_length(tree); ++i) {
        auto* wmClass = xcb_get_property_reply(connection,
            xcb_get_property(connection, false, children[i], XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 256), nullptr);
        const QString value = wmClass
            ? QString::fromLatin1(static_cast<const char*>(xcb_get_property_value(wmClass)), xcb_get_property_value_length(wmClass)).toLower()
            : QString{};
        free(wmClass);
        const bool keyboardWindow = value.contains(u"maliit"_s) || value.contains(u"onboard"_s)
            || value.contains(u"florence"_s) || value.contains(u"virtualkeyboard"_s);
        if (!keyboardWindow)
            continue;
        state.available = true;
        auto* attributes = xcb_get_window_attributes_reply(connection, xcb_get_window_attributes(connection, children[i]), nullptr);
        state.visible = attributes && attributes->map_state == XCB_MAP_STATE_VIEWABLE;
        free(attributes);
        if (state.visible)
            break;
    }
    free(tree);
    xcb_disconnect(connection);
    return state;
}
}

static bool groupMatches(const QString &group, const QStringList &patterns)
{
    return std::any_of(patterns.cbegin(), patterns.cend(), [&group](const auto &pattern) {
        if (pattern.isEmpty()) {
            return true;
        }

        if (pattern == group) {
            return true;
        }

        if (pattern.startsWith(group)) {
            return true;
        }

        if (pattern.endsWith(QLatin1Char('*')) && group.startsWith(pattern.left(pattern.length() - 1))) {
            return true;
        }

        return false;
    });
}

class VirtualKeyboardSettings : public SettingsModule
{
    Q_OBJECT
    static constexpr auto KEY_ACTIVE = "active"_L1;
    static constexpr auto KEY_ACTIVE_CLIENT_SUPPORTS_TEXT_INPUT = "activeClientSupportsTextInput"_L1;
    static constexpr auto KEY_AVAILABLE = "available"_L1;
    static constexpr auto KEY_ENABLED = "enabled"_L1;
    static constexpr auto KEY_VISIBLE = "visible"_L1;
    static constexpr auto KEY_WILL_SHOW_ON_ACTIVE = "willShowOnActive"_L1;

public:
    explicit VirtualKeyboardSettings(QObject* parent = nullptr)
        : SettingsModule(parent)
    {
    }

    inline QString group() final
    {
        return u"org.kde.VirtualKeyboard"_s;
    }

    VariantMapMap readAll(const QStringList &groups) final
    {
        Q_UNUSED(groups);
        const auto state = queryVirtualKeyboardState();
        VariantMapMap result;
        QVariantMap map{{KEY_ACTIVE, state.visible},
            {KEY_ACTIVE_CLIENT_SUPPORTS_TEXT_INPUT, false},
            {KEY_AVAILABLE, state.available},
            {KEY_ENABLED, state.available},
            {KEY_VISIBLE, state.visible},
            {KEY_WILL_SHOW_ON_ACTIVE, state.available}};
        result.insert(group(), map);
        return result;
    }

    QVariant read(const QString &group, const QString &key) final
    {
        return readAll({group}).value(this->group()).value(key);
    }
};

class TabletModeSettings : public SettingsModule
{
    Q_OBJECT
    static constexpr auto KEY_ENABLED = "enabled"_L1;
    static constexpr auto KEY_AVAILABLE = "available"_L1;

public:
    explicit TabletModeSettings(QObject* parent = nullptr)
        : SettingsModule(parent)
    {
    }

    inline QString group() final
    {
        return u"org.kde.TabletMode"_s;
    }

    VariantMapMap readAll(const QStringList &groups) final
    {
        Q_UNUSED(groups);
        const auto state = queryXInputDeviceState();
        qputenv("BREEZE_IS_TABLET_MODE", state.tabletModeEnabled ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
        VariantMapMap result;
        result.insert(group(), {{KEY_AVAILABLE, state.tabletModeAvailable || state.touchAvailable}, {KEY_ENABLED, state.tabletModeEnabled}});
        return result;
    }

    QVariant read(const QString &group, const QString &key) final
    {
        return readAll({group}).value(this->group()).value(key);
    }
};

/* accent-color */
struct AccentColorArray {
    double r = 0.0; // 0-1
    double g = 0.0; // 0-1
    double b = 0.0; // 0-1

    operator QVariant() const
    {
        return QVariant::fromValue(*this);
    }
};
Q_DECLARE_METATYPE(AccentColorArray)

QDBusArgument &operator<<(QDBusArgument &argument, const AccentColorArray &item)
{
    argument.beginStructure();
    argument << item.r << item.g << item.b;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, AccentColorArray &item)
{
    argument.beginStructure();
    argument >> item.r >> item.g >> item.b;
    argument.endStructure();
    return argument;
}

class FdoAppearanceSettings : public SettingsModule
{
    Q_OBJECT
    static constexpr auto colorScheme = "color-scheme"_L1;
    static constexpr auto accentColor = "accent-color"_L1;
    static constexpr auto reducedMotion = "reduced-motion"_L1;

public:
    explicit FdoAppearanceSettings(QObject *parent = nullptr)
        : SettingsModule(parent)
    {
        qDBusRegisterMetaType<AccentColorArray>();
        connect(qGuiApp, &QGuiApplication::paletteChanged, this, &FdoAppearanceSettings::onPaletteChanged);

        m_kdeglobalsWatcher = KConfigWatcher::create(KSharedConfig::openConfig(u"kdeglobals"_s));
        connect(m_kdeglobalsWatcher.get(), &KConfigWatcher::configChanged, this, &FdoAppearanceSettings::kdeglobalsChanged);
    }

    inline QString group() final
    {
        return u"org.freedesktop.appearance"_s;
    }

    VariantMapMap readAll(const QStringList &groups) final
    {
        Q_UNUSED(groups);
        VariantMapMap result;
        QVariantMap appearanceSettings;
        appearanceSettings.insert(colorScheme, readFdoColorScheme().variant());
        appearanceSettings.insert(accentColor, readAccentColor().variant());
        appearanceSettings.insert(reducedMotion, readReducedMotion().variant());
        result.insert(group(), appearanceSettings);
        return result;
    }

    QVariant read(const QString &group, const QString &key) final
    {
        if (group != this->group()) {
            return {};
        }
        if (key == colorScheme) {
            return readFdoColorScheme().variant();
        } else if (key == accentColor) {
            return readAccentColor().variant();
        } else if (key == reducedMotion) {
            return readReducedMotion().variant();
        }
        return {};
    }

private:
    QDBusVariant readFdoColorScheme()
    {
        const QPalette palette = QApplication::palette();
        const int windowBackgroundGray = qGray(palette.window().color().rgb());

        uint result = 0; // no preference

        if (windowBackgroundGray < 192) {
            result = 1; // prefer dark
        } else {
            result = 2; // prefer light
        }

        return QDBusVariant(result);
    }

    /**
     * Returns a list that contains redF, blueF and greenF and represents
     * the current accent color.
     * Format: (ddd)
     */
    QDBusVariant readAccentColor() const
    {
        const QColor color = qGuiApp->palette().highlight().color();
        return QDBusVariant(AccentColorArray{color.redF(), color.greenF(), color.blueF()});
    }

    QDBusVariant readReducedMotion() const
    {
        KConfig kdeglobals(u"kdeglobals"_s);
        const bool noMotion = kdeglobals.group(u"KDE"_s).readEntry("AnimationDurationFactor", 1.0) == 0;

        return QDBusVariant(noMotion ? 1U : 0U);
    }

private Q_SLOTS:
    void onPaletteChanged()
    {
        Q_EMIT settingChanged(group(), colorScheme, readFdoColorScheme());
        Q_EMIT settingChanged(group(), accentColor, readAccentColor());
    }

    void kdeglobalsChanged(const KConfigGroup &configGroup, const QByteArrayList &names)
    {
        if (configGroup.name() == u"KDE" && names.contains("AnimationDurationFactor")) {
            Q_EMIT settingChanged(group(), reducedMotion, readReducedMotion());
        }
    }

private:
    KConfigWatcher::Ptr m_kdeglobalsWatcher;
};

class KDEGlobalsSettings : public SettingsModule
{
    Q_OBJECT

    /**
     * An identifier for change signals.
     * @note Copied from KGlobalSettings
     */
    enum ChangeType {
        PaletteChanged = 0,
        FontChanged,
        StyleChanged,
        SettingsChanged,
        IconChanged,
        CursorChanged,
        ToolbarStyleChanged,
        ClipboardConfigChanged,
        BlockShortcuts,
        NaturalSortingChanged,
    };

    /**
     * Valid values for the settingsChanged signal
     * @note Copied from KGlobalSettings
     */
    enum SettingsCategory {
        SETTINGS_MOUSE,
        SETTINGS_COMPLETION,
        SETTINGS_PATHS,
        SETTINGS_POPUPMENU,
        SETTINGS_QT,
        SETTINGS_SHORTCUTS,
        SETTINGS_LOCALE,
        SETTINGS_STYLE,
    };

public:
    explicit KDEGlobalsSettings(QObject *parent = nullptr)
        : SettingsModule(parent)
        , m_kdeglobals(KSharedConfig::openConfig())

    {
        QDBusConnection::sessionBus().connect(QString(),
                                              QStringLiteral("/KDEPlatformTheme"),
                                              QStringLiteral("org.kde.KDEPlatformTheme"),
                                              QStringLiteral("refreshFonts"),
                                              this,
                                              SLOT(fontChanged()));
        QDBusConnection::sessionBus().connect(QString(),
                                              QStringLiteral("/KGlobalSettings"),
                                              QStringLiteral("org.kde.KGlobalSettings"),
                                              QStringLiteral("notifyChange"),
                                              this,
                                              // clang-format off
                                            SLOT(globalSettingChanged(int,int)));
        // clang-format on
        QDBusConnection::sessionBus().connect(QString(),
                                              QStringLiteral("/KToolBar"),
                                              QStringLiteral("org.kde.KToolBar"),
                                              QStringLiteral("styleChanged"),
                                              this,
                                              SLOT(toolbarStyleChanged()));
    }

    inline QString group() final
    {
        return u"org.kde.kdeglobals"_s;
    }

    VariantMapMap readAll(const QStringList &groups) final
    {
        VariantMapMap result;

        const auto groupList = m_kdeglobals->groupList();
        for (const QString &settingGroupName : groupList) {
            // NOTE: use org.kde.kdeglobals prefix

            QString uniqueGroupName = QStringLiteral("org.kde.kdeglobals.") + settingGroupName;

            if (!groupMatches(uniqueGroupName, groups)) {
                continue;
            }

            QVariantMap map;
            KConfigGroup configGroup(m_kdeglobals, settingGroupName);

            const auto keyList = configGroup.keyList();
            for (const QString &key : keyList) {
                map.insert(key, configGroup.readEntry(key));
            }

            result.insert(uniqueGroupName, map);
        }

        return result;
    }

    QVariant read(const QString &group, const QString &key) final
    {
        return readProperty(group, key).variant();
    }

private Q_SLOTS:
    void fontChanged()
    {
        Q_EMIT settingChanged(u"org.kde.kdeglobals.General"_s, u"font"_s, readProperty(u"org.kde.kdeglobals.General"_s, u"font"_s));
    }

    void globalSettingChanged(int type, int arg)
    {
        m_kdeglobals->reparseConfiguration();

        // Mostly based on plasma-integration needs
        switch (type) {
        case PaletteChanged:
            // Plasma-integration will be loading whole palette again, there is no reason to try to identify
            // particular categories or colors
            Q_EMIT settingChanged(u"org.kde.kdeglobals.General"_s, u"ColorScheme"_s, readProperty(u"org.kde.kdeglobals.General"_s, u"ColorScheme"_s));
            break;
        case FontChanged:
            fontChanged();
            break;
        case StyleChanged:
            Q_EMIT settingChanged(u"org.kde.kdeglobals.KDE"_s, u"widgetStyle"_s, readProperty(u"org.kde.kdeglobals.KDE"_s, u"widgetStyle"_s));
            break;
        case SettingsChanged: {
            auto category = SettingsCategory(arg);
            if (category == SETTINGS_QT || category == SETTINGS_MOUSE) {
                // TODO
            } else if (category == SETTINGS_STYLE) {
                // TODO
            }
            break;
        }
        case IconChanged:
            // we will get notified about each category, but it probably makes sense to send this signal just once
            if (arg == 0) { // KIconLoader::Desktop
                Q_EMIT settingChanged(u"org.kde.kdeglobals.Icons"_s, u"Theme"_s, readProperty(u"org.kde.kdeglobals.Icons"_s, u"Theme"_s));
            }
            break;
        case CursorChanged:
            // TODO
            break;
        case ToolbarStyleChanged:
            toolbarStyleChanged();
            break;
        default:
            break;
        }
    }

    void toolbarStyleChanged()
    {
        Q_EMIT settingChanged(u"org.kde.kdeglobals.Toolbar style"_s,
                              u"ToolButtonStyle"_s,
                              readProperty(u"org.kde.kdeglobals.Toolbar style"_s, u"ToolButtonStyle"_s));
    }

private:
    QDBusVariant readProperty(const QString &group, const QString &key)
    {
        static constexpr auto prefixLength = "org.kde.kdeglobals."_L1.length();
        QString groupName = group.right(group.length() - prefixLength);

        if (!m_kdeglobals->hasGroup(groupName)) {
            qCWarning(XdgDesktopPortalKdeSettings) << "Group " << group << " doesn't exist";
            return {};
        }

        KConfigGroup configGroup(m_kdeglobals, groupName);

        if (!configGroup.hasKey(key)) {
            qCWarning(XdgDesktopPortalKdeSettings) << "Key " << key << " doesn't exist";
            return {};
        }

        return QDBusVariant(configGroup.readEntry(key));
    }

    KSharedConfigPtr m_kdeglobals = KSharedConfig::openConfig();
};

SettingsPortal::SettingsPortal(DesktopPortal *parent)
    : QDBusAbstractAdaptor(parent)
    , m_parent(parent)
{
    m_settings.push_back(std::make_unique<FdoAppearanceSettings>(this));
    m_settings.push_back(std::make_unique<VirtualKeyboardSettings>(this));
    m_settings.push_back(std::make_unique<TabletModeSettings>(this));
    m_settings.push_back(std::make_unique<KDEGlobalsSettings>(this));
    for (const auto &setting : std::as_const(m_settings)) {
        connect(setting.get(), &SettingsModule::settingChanged, this, &SettingsPortal::SettingChanged);
    }
    qDBusRegisterMetaType<VariantMapMap>();
}

VariantMapMap SettingsPortal::ReadAll(const QStringList &groups)
{
    qCDebug(XdgDesktopPortalKdeSettings) << "ReadAll called with parameters:";
    qCDebug(XdgDesktopPortalKdeSettings) << "    groups: " << groups;

    VariantMapMap result;

    for (const auto &setting : m_settings) {
        if (groupMatches(setting->group(), groups)) {
            result.insert(setting->readAll(groups));
        }
    }

    return result;
}

QDBusVariant SettingsPortal::Read(const QString &group, const QString &key)
{
    qCDebug(XdgDesktopPortalKdeSettings) << "Read called with parameters:";
    qCDebug(XdgDesktopPortalKdeSettings) << "    group: " << group;
    qCDebug(XdgDesktopPortalKdeSettings) << "    key: " << key;

    auto sendError = [m = m_parent->message()](QDBusError::ErrorType error, const QString &message) {
        m.setDelayedReply(true);
        const auto reply = m.createErrorReply(error, message);
        QDBusConnection::sessionBus().send(reply);
    };

    auto setting = std::ranges::find(m_settings, group, [](const auto &setting) {
        return setting->group();
    });
    if (setting == std::ranges::end(m_settings)) {
        qCWarning(XdgDesktopPortalKdeSettings) << "Namespace " << group << " is not supported";
        sendError(QDBusError::UnknownProperty, QStringLiteral("Namespace is not supported"));
        return {};
    }

    const QVariant result = (*setting)->read(group, key);
    if (result.isNull()) {
        sendError(QDBusError::UnknownProperty, QStringLiteral("Property doesn't exist"));
        return {};
    }

    return QDBusVariant(result);
}

#include "moc_settings.cpp"
#include "settings.moc"
