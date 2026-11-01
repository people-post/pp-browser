# The app's feature-level composition (ProductStack): the conversations hub with calls and live
# broadcast as its participants; messaging's AI tools. The one feature module that sees conversations, calls
# and broadcast, and where messaging meets AI (CR007).
pp_browser_add_feature_library(pp_feature_product
  SOURCES
    BroadcastParticipant.cpp
    CallsParticipant.cpp
    MakeCallFunctionalPorts.cpp
    MessagingPeoplePickerPorts.cpp
    MessagingTools.cpp
    ProductStack.cpp
  PUBLIC_LIBS
    pp_feature_conversations
    pp_feature_calls
    pp_feature_broadcast
    pp_lib_ai)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
