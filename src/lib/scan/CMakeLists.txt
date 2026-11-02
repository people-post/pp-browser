# QR scanning (CR023 step 2): one camera scan at a time and the decoder, for the app's scan feature (gui/scan), on
# the media capability's camera lease.
pp_browser_add_lib_library(pp_lib_scan
  SOURCES
    QrDecode.cpp
    QrScanner.cpp
  PUBLIC_LIBS
    pp_lib_media
  PRIVATE_LIBS quirc)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
