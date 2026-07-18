cmake_minimum_required(VERSION 3.21)

set(CMAKE_C_STANDARD 99)

set(PROJECT_VERSION "6.7.3")

project(xdg-desktop-portal-kde VERSION ${PROJECT_VERSION})

set(PROJECT_DEP_VERSION "6.7.0")
set(QT_MIN_VERSION "6.10.0")
set(KF6_MIN_VERSION "6.26.0")
set(KDE_COMPILERSETTINGS_LEVEL "6.10.0")
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

################# set KDE specific information #################

find_package(ECM ${KF6_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEClangFormat)
include(KDEGitCommitHooks)
include(ECMSetupVersion)
include(ECMConfiguredInstall)
include(ECMQtDeclareLoggingCategory)
include(ECMAddTests)
include(ECMFindQmlModule)
include(ECMDeprecationSettings)
include(FeatureSummary)
include(ECMQmlModule)

find_package(Qt6 ${QT_MIN_VERSION} CONFIG REQUIRED COMPONENTS
    Core
    DBus
    PrintSupport
    QuickWidgets
    Widgets
    Quick
    QuickControls2
    Qml
    GuiPrivate
    Test
)

find_package(KF6 ${KF6_MIN_VERSION} REQUIRED
    CoreAddons
    Config
    I18n
    GlobalAccel
    KIO
    Kirigami
    Notifications
    Service
    WidgetsAddons
    WindowSystem
    IconThemes
    StatusNotifierItem
    Crash
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(XCB_COMPOSITE REQUIRED IMPORTED_TARGET xcb-composite)
pkg_check_modules(XCB_DAMAGE REQUIRED IMPORTED_TARGET xcb-damage)
pkg_check_modules(XCB_XFIXES REQUIRED IMPORTED_TARGET xcb-xfixes)
pkg_check_modules(XCB_RANDR REQUIRED IMPORTED_TARGET xcb-randr)
pkg_check_modules(XCB_SHM REQUIRED IMPORTED_TARGET xcb-shm)
pkg_check_modules(XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)
pkg_check_modules(XCB_XINPUT REQUIRED IMPORTED_TARGET xcb-xinput)
pkg_check_modules(XCB_EWMH REQUIRED IMPORTED_TARGET xcb-ewmh)
pkg_check_modules(XCB_IMAGE REQUIRED IMPORTED_TARGET xcb-image)
pkg_check_modules(XCB_RENDER REQUIRED IMPORTED_TARGET xcb-render)
pkg_check_modules(XCB_RENDERUTIL REQUIRED IMPORTED_TARGET xcb-renderutil)
pkg_check_modules(XKB REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(XKB_X11 REQUIRED IMPORTED_TARGET xkbcommon-x11)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBEI REQUIRED IMPORTED_TARGET libei-1.0)
pkg_check_modules(LIBEIS REQUIRED IMPORTED_TARGET libeis-1.0)
add_feature_info(XCB XCB_FOUND "Required for X11 capture, registry, and input")
add_feature_info(PIPEWIRE PIPEWIRE_FOUND "Required for PipeWire producer streams")
add_feature_info(EIS LIBEIS_FOUND "Required for EIS input backend")

find_package(KIOFuse)
set_package_properties(KIOFuse PROPERTIES
    URL https://commits.kde.org/system/kio-fuse
    TYPE RUNTIME
    PURPOSE "Automatic mounting of remote URLs")

ecm_setup_version(${PROJECT_VERSION}
    VARIABLE_PREFIX XDPK
    VERSION_HEADER ${CMAKE_BINARY_DIR}/version.h)

ecm_set_disabled_deprecation_versions(QT 5.15.0
    KF 6.4.0
)

add_subdirectory(data)
add_subdirectory(src)
add_subdirectory(autotests)

# add clang-format target for all our real source files
file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES *.cpp *.h)
list(FILTER ALL_CLANG_FORMAT_SOURCE_FILES EXCLUDE REGEX ".*\\.ccls-cache/.*")
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)

ki18n_install(po)

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)
