# Source layers: common → foundation → base(remaining domain peers) → domain → feature → app.
# `domain/` is added after `base/` while mesh identity still lives under base/mesh.
add_subdirectory(common)
add_subdirectory(foundation)
add_subdirectory(base)
add_subdirectory(domain)
# Migrated domain peers attach after their targets exist (base/ still owns pp_base).
if(TARGET pp_base AND TARGET pp_domain_people)
  target_link_libraries(pp_base INTERFACE pp_domain_people)
endif()
if(TARGET pp_base AND TARGET pp_domain_media)
  target_link_libraries(pp_base INTERFACE pp_domain_media)
endif()
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(feature)
endif()

if(PP_BROWSER_BUILD_TESTS)
  pp_browser_add_registered_tests()
endif()

add_subdirectory(app)
