# ISC QR decoder; see UPSTREAM.md for pinned source and import command.
add_library(quirc STATIC lib/quirc.c lib/decode.c lib/identify.c lib/version_db.c)
target_include_directories(quirc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set_target_properties(quirc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_link_libraries(quirc PRIVATE m)
endif()
# Dense codes (the link handshake's, QR version 29 to 30) have far more than the default 254 black and white blobs,
# and quirc numbers them top to bottom: past the limit the finder pattern at the bottom is never seen and nothing
# decodes, whatever the resolution. 16-bit region numbers cost one extra byte per pixel.
target_compile_definitions(quirc PRIVATE QUIRC_MAX_REGIONS=65534)
