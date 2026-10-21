# The app's feature-level composition (ProductStack): the conversations hub with calls and live
# broadcast as its participants. The one feature module that sees conversations, calls and broadcast.
pp_browser_add_feature_library(pp_feature_product
  SOURCES
    BroadcastParticipant.cpp
    CallsParticipant.cpp
    MakeCallFunctionalPorts.cpp
    ProductStack.cpp
  PUBLIC_LIBS
    pp_feature_conversations
    pp_feature_calls
    pp_feature_broadcast)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
