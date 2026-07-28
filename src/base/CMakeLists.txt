include(PpBrowserBase)

# Acyclic module order: each library links only lower layers.
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(error)
add_subdirectory(data)
add_subdirectory(crypto)
add_subdirectory(people)
add_subdirectory(messaging)
add_subdirectory(media)
add_subdirectory(net)
add_subdirectory(ai)
add_subdirectory(ui)

set(PP_BASE_MODULE_LIBS
  pp_base_error
  pp_base_platform_core
  pp_base_platform
  pp_base_data
  pp_base_i18n
  pp_base_crypto
  pp_base_people
  pp_base_messaging
  pp_base_media
  pp_base_net
  pp_base_ai
  pp_base_ai_conversation
  pp_base_ai_mcp
  pp_base_ui
)

add_library(pp_base INTERFACE)
target_link_libraries(pp_base INTERFACE ${PP_BASE_MODULE_LIBS})

add_library(pp_identity ALIAS pp_base)
