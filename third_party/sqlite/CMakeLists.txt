# SQLite amalgamation — upstream files imported by scripts/vendor_import.sh
add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_OMIT_DEPRECATED
)
if(UNIX)
  target_link_libraries(sqlite3 PRIVATE pthread)
  if(NOT APPLE)
    target_link_libraries(sqlite3 PRIVATE dl m)
  else()
    target_link_libraries(sqlite3 PRIVATE m)
  endif()
endif()

add_library(SQLite::SQLite3 ALIAS sqlite3)
