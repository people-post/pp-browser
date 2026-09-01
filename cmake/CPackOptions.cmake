include(GNUInstallDirs)
include(ProductBranding)

set(CPACK_PACKAGE_NAME "${PP_BROWSER_PRODUCT_SLUG}")
set(CPACK_PACKAGE_VENDOR "${PP_BROWSER_PRODUCT_SLUG}")
set(CPACK_PACKAGE_VERSION "${PP_BROWSER_RELEASE_VERSION}")
set(CPACK_PACKAGE_CONTACT "https://github.com/people-post/pp-browser")

if(WIN32)
  set(CPACK_GENERATOR "NSIS")
  set(CPACK_PACKAGE_FILE_NAME "${PP_BROWSER_PRODUCT_SLUG}-${PP_BROWSER_RELEASE_VERSION}-windows-x64")
  set(CPACK_NSIS_DISPLAY_NAME "${PP_BROWSER_PRODUCT_NAME}")
  set(CPACK_NSIS_PACKAGE_NAME "${PP_BROWSER_PRODUCT_NAME}")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  # Packaged Windows install puts the exe at $INSTDIR (not bin/).
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
  # Start Menu shortcut (always created).
  set(CPACK_PACKAGE_EXECUTABLES "${PP_BROWSER_PRODUCT_SLUG}" "${PP_BROWSER_PRODUCT_NAME}")
  # Desktop shortcut (always created). CPACK_CREATE_DESKTOP_LINKS alone is gated
  # behind the optional PATH/InstallOptions page, so create it explicitly.
  set(CPACK_NSIS_CREATE_ICONS_EXTRA
    "CreateShortCut '$DESKTOP\\\\${PP_BROWSER_PRODUCT_NAME}.lnk' '$INSTDIR\\\\${PP_BROWSER_PRODUCT_SLUG}.exe'")
  set(CPACK_NSIS_DELETE_ICONS_EXTRA
    "Delete '$DESKTOP\\\\${PP_BROWSER_PRODUCT_NAME}.lnk'")
elseif(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_PACKAGE_FILE_NAME "${PP_BROWSER_PRODUCT_SLUG}-${PP_BROWSER_RELEASE_VERSION}-macos")
  set(CPACK_DMG_VOLUME_NAME "${PP_BROWSER_PRODUCT_NAME}")
  set(CPACK_DMG_FORMAT "UDZO")
  # Package the signed PP.app only. A full-prefix copy would also ship bin/pp-node*
  # (separate release train), which fails notarization if left unsigned.
  # Release flow: cmake --install → sign-app → cpack.
  set(CPACK_INSTALL_CMAKE_PROJECTS "")
  set(CPACK_INSTALLED_DIRECTORIES
    "${CMAKE_INSTALL_PREFIX}/${PP_BROWSER_PRODUCT_NAME}.app"
    "${PP_BROWSER_PRODUCT_NAME}.app")
endif()

include(CPack)
