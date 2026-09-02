# Source layers: common → foundation → domain → base(remaining peers) → feature → app.
# PeerId lives in foundation/identity, so domain can build before remaining base peers.
add_subdirectory(common)
add_subdirectory(foundation)
add_subdirectory(domain)
add_subdirectory(base)
# Migrated domain peers attach onto pp_base after both trees exist.
if(TARGET pp_base AND TARGET pp_domain_people)
  target_link_libraries(pp_base INTERFACE pp_domain_people)
endif()
if(TARGET pp_base AND TARGET pp_domain_media)
  target_link_libraries(pp_base INTERFACE pp_domain_media)
endif()
if(TARGET pp_base AND TARGET pp_domain_net)
  target_link_libraries(pp_base INTERFACE pp_domain_net)
endif()
if(TARGET pp_base AND TARGET pp_domain_ui)
  target_link_libraries(pp_base INTERFACE pp_domain_ui)
endif()
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
