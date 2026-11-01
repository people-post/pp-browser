# The people capability (projects/capability-repos CR022): identity — keys, cards, contacts, devices, the locker
# format. Its src/ mirrors the tree it came from, so includes read as before ("lib/people/…"); this folder adds that
# src/ as an include root of its targets. It uses only the base below it (common, foundation, pp-cpp-*).

pp_browser_capability(people)

# Its common layer: the phone book's vocabulary (directory JSON, key cards, people-discovery blocks).
add_library(pp_people_common STATIC
  src/common/directory/DirectoryJson.cpp
  src/common/directory/KeyCard.cpp
  src/common/directory/PeopleDiscoveryBlocks.cpp)
target_include_directories(pp_people_common PUBLIC ${PP_BROWSER_CAPABILITY_SRC})
target_link_libraries(pp_people_common PUBLIC pp_pbr_common)
pp_browser_apply_warnings(pp_people_common)

add_subdirectory(src/lib/people)

# Domain modules need the full product libs (not in headless configures).
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/domain/peers)
endif()
