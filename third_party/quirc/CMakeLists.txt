# ISC QR decoder; see UPSTREAM.md for pinned source and import command.
add_library(quirc STATIC lib/quirc.c lib/decode.c lib/identify.c lib/version_db.c)
target_include_directories(quirc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/lib)
set_target_properties(quirc PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(UNIX)
  target_link_libraries(quirc PRIVATE m)
endif()
