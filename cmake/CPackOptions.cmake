include(GNUInstallDirs)

set(CPACK_PACKAGE_NAME "frame")
set(CPACK_PACKAGE_VENDOR "Frame")
set(CPACK_PACKAGE_VERSION "${PP_BROWSER_RELEASE_VERSION}")
set(CPACK_PACKAGE_CONTACT "https://github.com/people-post/pp-browser")

if(WIN32)
  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_FILE_NAME "frame-${PP_BROWSER_RELEASE_VERSION}-windows-x64")
  set(CPACK_NSIS_DISPLAY_NAME "Frame")
  set(CPACK_NSIS_PACKAGE_NAME "Frame")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  # Packaged Windows install puts the exe at $INSTDIR (not bin/).
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
  # Start Menu shortcut (always created).
  set(CPACK_PACKAGE_EXECUTABLES "pp-browser" "Frame")
  # Desktop shortcut (always created). CPACK_CREATE_DESKTOP_LINKS alone is gated
  # behind the optional PATH/InstallOptions page, so create it explicitly.
  set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "CreateShortCut '$DESKTOP\\\\Frame.lnk' '$INSTDIR\\\\pp-browser.exe'")
  set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "Delete '$DESKTOP\\\\Frame.lnk'")
elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_PACKAGE_FILE_NAME "frame-${PP_BROWSER_RELEASE_VERSION}-macos")
  set(CPACK_DMG_VOLUME_NAME "Frame")
  set(CPACK_DMG_FORMAT "UDZO")
endif()

include(CPack)
