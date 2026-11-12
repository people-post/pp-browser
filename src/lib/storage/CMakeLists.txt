# The app's storage (projects/capability-repos CR024): the profile database — profile.db and the per-thread files,
# their schema and migrations, at-rest encryption — and the SQLite implementations of the capabilities' store ports.
# Capabilities ship ideas and in-memory stores; only the app links this.
pp_browser_add_lib_library(pp_lib_storage
  SOURCES
    ProfileStorage.cpp
    SqliteCallMediaKeyStore.cpp
    SqliteCallSessionStore.cpp
    SqliteChatDbLadders.cpp
    SqliteChatStoreGate.cpp
    SqliteControlLaneSeenStore.cpp
    SqliteGroupInviteLinkStore.cpp
    SqliteGroupRosterStore.cpp
    SqliteMigrations.cpp
    SqliteObjectIndex.cpp
    SqlitePeerCardStore.cpp
    SqlitePeerSessionStore.cpp
    SqlitePostStore.cpp
    SqliteThreadDb.cpp
    SqliteThreadLocalPrefsStore.cpp
    SqliteThreadMentionsStore.cpp
    SqliteThreadSchema.cpp
    SqliteThreadStore.cpp
  PUBLIC_LIBS
    pp_lib_messaging
    pp_lib_calls
    pp_lib_posts
    pp_lib_content
    pp_lib_delivery
    pp_foundation_crypto
    pp_foundation_error
    sodium
    SQLite::SQLite3)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
