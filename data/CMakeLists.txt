configure_file(org.freedesktop.impl.portal.desktop.kde.desktop.in org.freedesktop.impl.portal.desktop.kde.desktop @ONLY)
configure_file(org.freedesktop.impl.portal.desktop.sonicde.desktop.in org.freedesktop.impl.portal.desktop.sonicde.desktop @ONLY)
configure_file(org.freedesktop.impl.portal.desktop.kde.cmake.in org.freedesktop.impl.portal.desktop.kde.service @ONLY)
configure_file(org.freedesktop.impl.portal.desktop.sonicde.cmake.in org.freedesktop.impl.portal.desktop.sonicde.service @ONLY)

install(FILES kde.portal DESTINATION ${KDE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES sonicde.portal DESTINATION ${KDE_INSTALL_DATADIR}/xdg-desktop-portal/portals)
install(FILES sonicde-portals.conf DESTINATION ${KDE_INSTALL_DATADIR}/xdg-desktop-portal/)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.kde.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.sonicde.service DESTINATION ${KDE_INSTALL_DBUSSERVICEDIR})

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.kde.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.sonicde.desktop DESTINATION ${KDE_INSTALL_APPDIR})

ecm_install_configured_files(INPUT plasma-xdg-desktop-portal-kde.service.in @ONLY
            DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})
ecm_install_configured_files(INPUT plasma-xdg-desktop-portal-sonicde.service.in @ONLY
            DESTINATION ${KDE_INSTALL_SYSTEMDUSERUNITDIR})