# Install rules and CPack packages (TGZ everywhere, DEB on Debian-like Linux).
include(GNUInstallDirs)

if(LANSYNC_BUILD_CLI)
    install(TARGETS lansync-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(LANSYNC_BUILD_DESKTOP)
    install(TARGETS lansync RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(UNIX AND NOT APPLE)
        install(FILES ${CMAKE_SOURCE_DIR}/packaging/lansync.desktop
                DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    endif()
endif()
install(FILES ${CMAKE_SOURCE_DIR}/README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})

set(CPACK_PACKAGE_NAME "lansync")
set(CPACK_PACKAGE_VENDOR "LanSync")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Secure LAN file synchronization")
set(CPACK_GENERATOR "TGZ")
if(UNIX AND NOT APPLE)
    find_program(LANSYNC_DPKG dpkg-deb)
    if(LANSYNC_DPKG)
        list(APPEND CPACK_GENERATOR "DEB")
        set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LanSync maintainers")
        set(CPACK_DEBIAN_PACKAGE_SECTION "net")
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
        # QML modules are loaded at runtime, so shlibdeps cannot see them.
        set(CPACK_DEBIAN_PACKAGE_DEPENDS
            "qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-layouts, qml6-module-qtquick-window, qml6-module-qtquick-templates, qml6-module-qtquick-dialogs, qml6-module-qtqml-workerscript")
    endif()
endif()
include(CPack)
