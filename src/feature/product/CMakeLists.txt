# The app's feature-level composition (ProductStack): the mesh for the profile's role (ProductMesh,
# over domain/node), the conversations hub with calls and live broadcast as its participants, and
# messaging's AI tools. The one feature module that sees conversations, calls and broadcast, and
# where messaging meets AI (CR007) and the node role.
pp_browser_add_feature_library(pp_feature_product
  SOURCES
    BroadcastParticipant.cpp
    CallsParticipant.cpp
    MakeCallFunctionalPorts.cpp
    MessagingPeoplePickerPorts.cpp
    MessagingTools.cpp
    HubSettingsProjection.cpp
    ProductMesh.cpp
    ProductOwners.cpp
    ProductStack.cpp
  PUBLIC_LIBS
    pp_feature_conversations
    pp_feature_calls
    pp_feature_broadcast
    pp_lib_ai
    pp_domain_node
    pp_lib_storage
    pp_foundation_platform)

if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(tests)
endif()
