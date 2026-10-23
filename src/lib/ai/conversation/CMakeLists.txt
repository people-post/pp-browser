pp_browser_add_lib_library(pp_lib_ai_conversation
  SOURCES
    Conversation.cpp
    SlidingWindowContextPolicy.cpp
    ThreadCompactionService.cpp
    ThreadContextPolicy.cpp
    TurnCoordinator.cpp
    UserMessageFormatter.cpp
  PUBLIC_LIBS
    pp_lib_ai
    pp_foundation_data
    pp_foundation_platform
    CURL::libcurl)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
