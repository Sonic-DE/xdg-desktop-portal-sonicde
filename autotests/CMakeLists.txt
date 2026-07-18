ecm_add_tests(
    xdgshortcuttest.cpp

    LINK_LIBRARIES Qt::Test Qt::Gui
)
target_sources(xdgshortcuttest PRIVATE ${CMAKE_SOURCE_DIR}/src/xdgshortcut.cpp ${CMAKE_SOURCE_DIR}/src/xdgshortcut.h)
target_link_libraries(xdgshortcuttest Qt::GuiPrivate)

add_test(
    NAME colorschemetest
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/colorschemetest.py
)
set_tests_properties(colorschemetest PROPERTIES ENVIRONMENT "CMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set_tests_properties(colorschemetest PROPERTIES
    ENVIRONMENT "CMAKE_RUNTIME_OUTPUT_DIRECTORY=$<TARGET_FILE_DIR:xdg-desktop-portal-sonicde>;PORTAL_SERVICE_NAME=org.freedesktop.impl.portal.desktop.sonicde;PORTAL_EXECUTABLE_NAME=xdg-desktop-portal-sonicde"
    LABELS "unit"
)

ecm_add_test(inputcapturebarriertest.cpp ${CMAKE_SOURCE_DIR}/src/inputcapturebarrier.cpp TEST_NAME inputcapturebarriertest LINK_LIBRARIES Qt::Test Qt::Gui)

# Pure unit tests for the X11 replacement components
set(_x11_unit_tests
    x11capturetest
    pipewirestreamtest
    x11inputtest
    xeismountertest
    x11clipboardprotocoltest
    portalhelperstest
)

foreach(_test IN LISTS _x11_unit_tests)
    ecm_add_test(${_test}.cpp
        TEST_NAME ${_test}
        LINK_LIBRARIES
            Qt::Test
            Qt::Core
            Qt::Gui
            Qt::DBus
            Qt::Qml
            xdg-desktop-portal-kde-backend
            PkgConfig::XCB
            PkgConfig::XCB_COMPOSITE
            PkgConfig::XCB_XFIXES
            PkgConfig::XCB_RANDR
            PkgConfig::XCB_XTEST
            PkgConfig::XKB
            PkgConfig::XKB_X11
            PkgConfig::PIPEWIRE
            PkgConfig::LIBEIS
    )
    target_include_directories(${_test} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    set_tests_properties(${_test} PROPERTIES LABELS "unit")
endforeach()

set(_test_portal_prefix ${CMAKE_CURRENT_BINARY_DIR}/test-prefix)
set(_test_portal_dir ${_test_portal_prefix}/share/xdg-desktop-portal/portals)
set(_test_config_dir ${_test_portal_prefix}/share/xdg-desktop-portal)
set(_test_dbus_dir ${_test_portal_prefix}/share/dbus-1/services)
file(MAKE_DIRECTORY ${_test_portal_dir} ${_test_config_dir} ${_test_dbus_dir})
configure_file(test-sonicde.portal.in ${_test_portal_dir}/sonicde-test.portal @ONLY)
configure_file(test-sonicde-portals.conf.in ${_test_config_dir}/sonicde-portals.conf @ONLY)
configure_file(org.freedesktop.impl.portal.desktop.sonicde.test.service.in ${_test_dbus_dir}/org.freedesktop.impl.portal.desktop.sonicde.test.service @ONLY)

add_executable(xdg-desktop-portal-sonicde-test screencasttestbackend.cpp)
target_include_directories(xdg-desktop-portal-sonicde-test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(xdg-desktop-portal-sonicde-test PRIVATE
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
    KF6::Crash
    PkgConfig::PIPEWIRE
)

add_executable(directbackendclient directbackendclient.cpp)
target_link_libraries(directbackendclient PRIVATE
    Qt::Core
    Qt::DBus
    Qt::Gui
    Qt::Widgets
    PkgConfig::PIPEWIRE
)

add_test(NAME directbackendproducertest COMMAND directbackendclient)
set_tests_properties(directbackendproducertest PROPERTIES
    ENVIRONMENT "XDP_SONICDE_TEST_BACKEND=$<TARGET_FILE:xdg-desktop-portal-sonicde-test>;QT_QPA_PLATFORM=xcb;XDG_CURRENT_DESKTOP=SONICDE"
    LABELS "live;direct-producer"
    SKIP_RETURN_CODE 77
)

add_test(NAME sandboxfrontendtest COMMAND directbackendclient)
set_tests_properties(sandboxfrontendtest PROPERTIES
    ENVIRONMENT "XDP_SONICDE_TEST_BACKEND=$<TARGET_FILE:xdg-desktop-portal-sonicde-test>;QT_QPA_PLATFORM=xcb;XDG_CURRENT_DESKTOP=SONICDE"
    LABELS "live;sandbox"
    SKIP_RETURN_CODE 77
)

add_test(NAME manualchooservalidationtest COMMAND directbackendclient)
set_tests_properties(manualchooservalidationtest PROPERTIES
    ENVIRONMENT "XDP_SONICDE_TEST_BACKEND=$<TARGET_FILE:xdg-desktop-portal-sonicde-test>;QT_QPA_PLATFORM=xcb;XDG_CURRENT_DESKTOP=SONICDE"
    LABELS "manual;interactive"
    SKIP_RETURN_CODE 77
)
