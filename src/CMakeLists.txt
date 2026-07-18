add_definitions(-DTRANSLATION_DOMAIN="xdg-desktop-portal")

add_subdirectory(kirigami-filepicker)

# Backend sources shared by both executables.
set(_xdp_backend_srcs
    access.cpp
    accessdialog.cpp
    accessdialog.h
    access.h
    account.cpp
    account.h
    appchooser.cpp
    appchooserdialog.cpp
    appchooserdialog.h
    appchooser.h
    background.cpp
    background.h
    clipboard.cpp
    clipboard.h
    dbushelpers.cpp dbushelpers.h
    desktopportal.cpp
    desktopportal.h
    email.cpp
    email.h
    filechooser.cpp
    filechooser.h
    globalshortcuts.cpp globalshortcuts.h
    inhibit.cpp
    inhibit.h
    inputcapture.cpp
    inputcapturebarrier.cpp
    inputcapturedialog.cpp
    notificationinhibition.cpp
    notificationinhibition.h
    outputsmodel.cpp outputsmodel.h
    pendingportalreply.cpp pendingportalreply.h
    print.cpp
    print.h
    quickdialog.cpp quickdialog.h
    remotedesktop.cpp
    remotedesktopdialog.cpp
    remotedesktopdialog.h
    remotedesktop.h
    restoredata.h
    restoredata.cpp
    request.cpp
    request.h
    screencast.cpp
    screencast.h
    screenselectionprovider.cpp
    screenselectionprovider.h
    screencasting.cpp
    screencasting.h
    screenchooserdialog.cpp
    screenchooserdialog.h
    screenshot.cpp
    screenshotdialog.cpp
    screenshotdialog.h
    screenshot.h
    session.cpp
    session.h
    settings.cpp
    settings.h
    userinfodialog.cpp
    userinfodialog.h
    usb.cpp
    utils.cpp
    utils.h
    portalicon.cpp portalicon.h
    dynamiclauncher.cpp dynamiclauncher.h
    dynamiclauncherdialog.cpp dynamiclauncherdialog.h
    xdgshortcut.cpp xdgshortcut.h
    region-select/SelectionEditor.h
    region-select/SelectionEditor.cpp
    wallpaper.cpp
    x11/x11controller.cpp
    x11/x11controller.h
    x11/x11worker.cpp
    x11/x11worker.h
    x11/x11capture.cpp
    x11/x11capture.h
    x11/x11input.cpp
    x11/x11input.h
    x11/x11clipboard.cpp
    x11/x11clipboard.h
    x11/pipewirestream.cpp
    x11/pipewirestream.h
    x11/xeismounter.cpp
    x11/xeismounter.h
    x11portalbootstrap.cpp
    x11portalbootstrap.h
)

# Static backend library
add_library(xdg-desktop-portal-kde-backend STATIC ${_xdp_backend_srcs})
if(BUILD_TESTING)
    target_compile_definitions(xdg-desktop-portal-kde-backend PUBLIC XDP_BUILD_TESTING)
endif()
target_link_libraries(xdg-desktop-portal-kde-backend
PRIVATE
    Qt::Core
    Qt::DBus
    Qt::PrintSupport
    Qt::QuickWidgets
    Qt::Widgets
    Qt::GuiPrivate
    KF6::CoreAddons
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::GlobalAccel
    KF6::I18n
    KF6::I18nQml
    KF6::KIOFileWidgets
    KF6::Notifications
    KF6::Service
    KF6::WidgetsAddons
    KF6::WindowSystem
    KF6::IconThemes
    KF6::StatusNotifierItem
    KF6::Crash
    KirigamiFilepicker
    PkgConfig::XCB
    PkgConfig::XCB_COMPOSITE
    PkgConfig::XCB_DAMAGE
    PkgConfig::XCB_XFIXES
    PkgConfig::XCB_RANDR
    PkgConfig::XCB_SHM
    PkgConfig::XCB_XTEST
    PkgConfig::XCB_XINPUT
    PkgConfig::XCB_EWMH
    PkgConfig::XCB_IMAGE
    PkgConfig::XCB_RENDER
    PkgConfig::XCB_RENDERUTIL
    PkgConfig::XKB
    PkgConfig::XKB_X11
    PkgConfig::PIPEWIRE
    PkgConfig::LIBEI
    PkgConfig::LIBEIS)

# Logging categories for the backend.
macro(xdp_add_logging_category _id _cat _hdr _desc)
    ecm_qt_declare_logging_category(_xdp_logging_srcs
        IDENTIFIER "${_id}"
        CATEGORY_NAME "${_cat}"
        HEADER "${_hdr}"
        DESCRIPTION "${_desc}"
        EXPORT XDPKDE
    )
endmacro()
xdp_add_logging_category(XdgDesktopPortalKdeBackground xdp-kde-background background_debug.h "XDG Desktop Portal KDE Background")
xdp_add_logging_category(XdgDesktopPortalKdeNotificationInhibition xdp-kde-notificationinhibition notificationinhibition_debug.h "XDG Desktop Portal KDE Notification Inhibition")
xdp_add_logging_category(XdgDesktopPortalKdeAccess xdp-kde-access access_debug.h "XDG Desktop Portal KDE Access")
xdp_add_logging_category(XdgDesktopPortalKdeAccount xdp-kde-account account_debug.h "XDG Desktop Portal KDE Account")
xdp_add_logging_category(XdgDesktopPortalKdeSettings xdp-kde-settings settings_debug.h "XDG Desktop Portal KDE Settings")
xdp_add_logging_category(XdgDesktopPortalKdeAppChooser xdp-kde-app-chooser appchooser_debug.h "XDG Desktop Portal KDE App Chooser")
xdp_add_logging_category(XdgDesktopPortalKdeDesktopPortal xdp-kde-desktop-portal desktopportal_debug.h "XDG Desktop Portal KDE Desktop Portal")
xdp_add_logging_category(XdgDesktopPortalKdeDynamicLauncher xdp-kde-dynamiclauncher dynamiclauncher_debug.h "XDG Desktop Portal KDE Dynamic Launcher")
xdp_add_logging_category(XdgDesktopPortalKdeDynamicLauncherDialog xdp-kde-dynamic-launcher-dialog dynamiclauncherdialog_debug.h "XDG Desktop Portal KDE Dynamic Launcher Dialog")
xdp_add_logging_category(XdgDesktopPortalKdeEmail xdp-kde-email email_debug.h "XDG Desktop Portal KDE e-mail")
xdp_add_logging_category(XdgDesktopPortalKdeFileChooser xdp-kde-filechooser filechooser_debug.h "XDG Desktop Portal KDE File Chooser")
xdp_add_logging_category(XdgDesktopPortalKdeInhibit xdp-kde-inhibit inhibit_debug.h "XDG Desktop Portal KDE Inhibit")
xdp_add_logging_category(XdgDesktopPortalKdePrint xdp-kde-print print_debug.h "XDG Desktop Portal KDE Print")
xdp_add_logging_category(XdgDesktopPortalKdeRemoteDesktop xdp-kde-remotedesktop remotedesktop_debug.h "XDG Desktop Portal KDE Remote Desktop")
xdp_add_logging_category(XdgDesktopPortalKdeRemoteDesktopDialog xdp-kde-remote-desktop-dialog remotedesktopdialog_debug.h "XDG Desktop Portal KDE Remote Desktop Dialog")
xdp_add_logging_category(XdgRequestKdeRequest xdp-kde-request request_debug.h "XDG Desktop Portal KDE Request")
xdp_add_logging_category(XdgDesktopPortalKdeScreenCast xdp-kde-screencast screencast_debug.h "XDG Desktop Portal KDE Screencast")
xdp_add_logging_category(XdgDesktopPortalKdeScreenshot xdp-kde-screenshot screenshot_debug.h "XDG Desktop Portal KDE Screenshot")
xdp_add_logging_category(XdgDesktopPortalKdeScreenshotDialog xdp-kde-screenshot-dialog screenshotdialog_debug.h "XDG Desktop Portal KDE Screenshot Dialog")
xdp_add_logging_category(XdgSessionKdeSession xdp-kde-session session_debug.h "XDG Desktop Portal KDE Session")
xdp_add_logging_category(XdgDesktopPortalKde xdp-kde debug.h "XDG Desktop Portal KDE Generic")
xdp_add_logging_category(XdgDesktopPortalKdeX11 xdp-kde-x11 x11_debug.h "XDG Desktop Portal KDE X11 Backend")
xdp_add_logging_category(XdgDesktopPortalKdeAccessDialog xdp-kde-access-dialog accessdialog_debug.h "XDG Desktop Portal KDE Access Dialog")
xdp_add_logging_category(XdgDesktopPortalKdeInputCapture xdp-kde-inputcapture inputcapture_debug.h "XDG Desktop Portal KDE Input Capture")
xdp_add_logging_category(XdgDesktopPortalKdeClipboard xdp-kde-clipboard clipboard_debug.h "XDG Desktop Portal KDE Clipboard")
xdp_add_logging_category(XdgDesktopPortalKdeWallpaper xdp-kde-wallpaper wallpaper_debug.h "XDG Desktop Portal KDE Wallpaper")
target_sources(xdg-desktop-portal-kde-backend PRIVATE ${_xdp_logging_srcs})

# D-Bus generated sources for backend
set(_xdp_backend_gen_srcs)
set(kglobalaccel_xml ${KGLOBALACCEL_DBUS_INTERFACES_DIR}/kf6_org.kde.KGlobalAccel.xml)
set_source_files_properties(${kglobalaccel_xml} PROPERTIES
    INCLUDE "kglobalaccel.h"
    NO_NAMESPACE TRUE
    CLASSNAME "KGlobalAccelInterface"
)
qt_add_dbus_interface(_xdp_backend_gen_srcs ${kglobalaccel_xml} kglobalaccel_interface)

set(kglobalaccel_component_xml ${KGLOBALACCEL_DBUS_INTERFACES_DIR}/kf6_org.kde.kglobalaccel.Component.xml)
set_source_files_properties(${kglobalaccel_component_xml} PROPERTIES
    INCLUDE "kglobalshortcutinfo.h"
    NO_NAMESPACE TRUE
    CLASSNAME "KGlobalAccelComponentInterface"
)
qt_add_dbus_interface(_xdp_backend_gen_srcs ${kglobalaccel_component_xml} kglobalaccel_component_interface)

qt_add_dbus_adaptor(_xdp_backend_gen_srcs org.freedesktop.impl.portal.Session.xml session.h Session)

set_source_files_properties(../data/org.freedesktop.Accounts.User.xml PROPERTIES NO_NAMESPACE TRUE)
qt_add_dbus_interface(_xdp_backend_gen_srcs ../data/org.freedesktop.Accounts.User.xml user_interface)

set_source_files_properties(../data/org.freedesktop.portal.Documents.xml PROPERTIES NO_NAMESPACE TRUE)
qt_add_dbus_interface(_xdp_backend_gen_srcs ../data/org.freedesktop.portal.Documents.xml documents_interface)

set_source_files_properties(../data/org.kde.KIOFuse.VFS.xml PROPERTIES NO_NAMESPACE TRUE)
qt_add_dbus_interface(_xdp_backend_gen_srcs ../data/org.kde.KIOFuse.VFS.xml fuse_interface)

qt_add_dbus_interface(_xdp_backend_gen_srcs ../data/org.freedesktop.Application.xml fdo_application_interface)

set_source_files_properties(../data/org.freedesktop.impl.portal.PermissionStore.xml PROPERTIES NO_NAMESPACE TRUE INCLUDE "dbushelpers.h")
qt_add_dbus_interface(_xdp_backend_gen_srcs ../data/org.freedesktop.impl.portal.PermissionStore.xml permission_store)

target_sources(xdg-desktop-portal-kde-backend PRIVATE ${_xdp_backend_gen_srcs})

# QML module on backend library
ecm_add_qml_module(xdg-desktop-portal-kde-backend
    URI org.kde.xdgdesktopportal
    VERSION 1.0
    GENERATE_PLUGIN_SOURCE)
ecm_target_qml_sources(xdg-desktop-portal-kde-backend
    SOURCES
        ScreenChooserDialog.qml
        UserInfoDialog.qml
        AccessDialog.qml
        AppChooserDialog.qml
        RemoteDesktopDialog.qml
        ScreenshotDialog.qml
        DynamicLauncherDialog.qml
        PipeWireDelegate.qml
        InputCaptureDialog.qml
        GlobalShortcutsDialog.qml
        WallpaperDialog.qml
        UsbDialog.qml
        PortalDialog.qml
        PipeWireLayout.qml
        ScreenChooserDialogTemplate.qml
        region-select/RegionSelectOverlay.qml
        region-select/FloatingTextBox.qml
        region-select/FloatingBackground.qml)

# Function that adds one executable with a given identity.
function(add_xdg_desktop_portal_exec _target _service _object_path _desktop_file _main_src)
    add_executable(${_target} ${_main_src})
    target_compile_definitions(${_target} PRIVATE
        XDPSONICDE_SERVICE_NAME_LITERAL="${_service}"
        QDPSONICDE_SERVICE_NAME_LITERAL="${_service}"
        QDPSONICDE_OBJECT_PATH_LITERAL="${_object_path}"
        QDPSONICDE_DESKTOP_FILE_LITERAL="${_desktop_file}")
    target_link_libraries(${_target} PRIVATE
        xdg-desktop-portal-kde-backend
        Qt::Core
        Qt::DBus
        Qt::Gui
        Qt::Widgets
        Qt::PrintSupport
        Qt::QuickWidgets
        Qt::Quick
        Qt::Qml
        KF6::CoreAddons
        KF6::ConfigCore
        KF6::ConfigGui
        KF6::GlobalAccel
        KF6::I18n
        KF6::I18nQml
        KF6::Notifications
        KF6::Service
        KF6::WidgetsAddons
        KF6::WindowSystem
        KF6::IconThemes
        KF6::StatusNotifierItem
        KF6::Crash)
    install(TARGETS ${_target} DESTINATION ${KDE_INSTALL_LIBEXECDIR})
endfunction()

# KDE executable preserves the original identity.
add_xdg_desktop_portal_exec(xdg-desktop-portal-kde
    "org.freedesktop.impl.portal.desktop.kde"
    "/org/freedesktop/portal/desktop"
    "org.freedesktop.impl.portal.desktop.kde.desktop"
    ${CMAKE_CURRENT_SOURCE_DIR}/xdg-desktop-portal-kde.cpp)

# SonicDE executable introduces the new identity.
add_xdg_desktop_portal_exec(xdg-desktop-portal-sonicde
    "org.freedesktop.impl.portal.desktop.sonicde"
    "/org/freedesktop/portal/desktop"
    "org.freedesktop.impl.portal.desktop.sonicde.desktop"
    ${CMAKE_CURRENT_SOURCE_DIR}/xdg-desktop-portal-kde.cpp)

# Logging categories installation (exported under XDPKDE).
ecm_qt_install_logging_categories(EXPORT XDPKDE FILE xdp-kde.categories DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR})

# Notifyrc install.
install(FILES xdg-desktop-portal-kde.notifyrc DESTINATION ${KDE_INSTALL_KNOTIFYRCDIR})
