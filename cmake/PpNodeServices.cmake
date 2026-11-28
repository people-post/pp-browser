# The node's optional services, chosen at compile time (projects/node-relay NR001). pp-node and pp-node-plus are the two
# sets we build and release; an operator may build either binary with any other set. Nothing on the wire, on disk or
# in the config names a set: clients find a service by what a node advertises.
#
# A service is a capability library that defines PP_NODE_HAS_<SERVICE> (PUBLIC), so the binary that links it starts it
# (apps/node/src/app/node/main.cpp). Its code is in its own namespace, which is what the symbol checks look for.

# Every service a node can be built with, in the capability order. For each: its library, what it needs, and what
# tells its code apart in a symbol listing (a mangled-name fragment).
set(PP_NODE_KNOWN_SERVICES ledger relay)
set(PP_NODE_SERVICE_LIB_ledger pp_domain_ledger) # the ledger role (capabilities/ledger), PP_NODE_HAS_LEDGER
set(PP_NODE_SERVICE_NEEDS_ledger "")
set(PP_NODE_SERVICE_SYMBOL_ledger "3pbr6ledger")
set(PP_NODE_SERVICE_LIB_relay pp_domain_relay) # the message relay (capabilities/relay), PP_NODE_HAS_RELAY
set(PP_NODE_SERVICE_NEEDS_relay "")
set(PP_NODE_SERVICE_SYMBOL_relay "3pbr5relay")

# The two sets we provide. The ledger is core, but only where it builds (PP_BROWSER_WITH_LEDGER; not on mobile).
if(TARGET pp_domain_ledger)
  set(PP_NODE_PRESET_node ledger)
else()
  set(PP_NODE_PRESET_node "")
endif()
set(PP_NODE_PRESET_plus ${PP_NODE_PRESET_node} relay)

set(PP_NODE_SERVICES "${PP_NODE_PRESET_node}" CACHE STRING
  "Services built into pp-node (;-list of ${PP_NODE_KNOWN_SERVICES}; default: the node preset)")
set(PP_NODE_PLUS_SERVICES "${PP_NODE_PRESET_plus}" CACHE STRING
  "Services built into pp-node-plus (;-list of ${PP_NODE_KNOWN_SERVICES}; default: the plus preset)")

# pp_node_services(<services> OUT_LIBS v OUT_ALLOW v OUT_REQUIRE v OUT_FORBID v OUT_EDITION v): checks a set (known
# names, each one's needs in the set, its library built) and returns its libraries, their allowlist patterns, the
# symbols the binary must and must not have, and its edition: node, plus, or custom.
function(pp_node_services services)
  cmake_parse_arguments(ARG "" "OUT_LIBS;OUT_ALLOW;OUT_REQUIRE;OUT_FORBID;OUT_EDITION" "" ${ARGN})
  set(_libs "")
  set(_allow "")
  set(_require "")
  set(_forbid "")
  foreach(_service IN LISTS services)
    if(NOT _service IN_LIST PP_NODE_KNOWN_SERVICES)
      message(FATAL_ERROR "node services: no service '${_service}' (services: ${PP_NODE_KNOWN_SERVICES})")
    endif()
    foreach(_need IN LISTS PP_NODE_SERVICE_NEEDS_${_service})
      if(NOT _need IN_LIST services)
        message(FATAL_ERROR "node services: ${_service} needs ${_need}; add it to the set (${services})")
      endif()
    endforeach()
    if(NOT TARGET ${PP_NODE_SERVICE_LIB_${_service}})
      message(FATAL_ERROR "node services: ${_service} is not built here (${PP_NODE_SERVICE_LIB_${_service}}; "
        "the ledger needs PP_BROWSER_WITH_LEDGER=ON)")
    endif()
    list(APPEND _libs ${PP_NODE_SERVICE_LIB_${_service}})
    list(APPEND _allow "^${PP_NODE_SERVICE_LIB_${_service}}$")
  endforeach()
  foreach(_service IN LISTS PP_NODE_KNOWN_SERVICES)
    if(_service IN_LIST services)
      list(APPEND _require ${PP_NODE_SERVICE_SYMBOL_${_service}})
    else()
      list(APPEND _forbid ${PP_NODE_SERVICE_SYMBOL_${_service}})
    endif()
  endforeach()
  set(_sorted ${services})
  set(_node ${PP_NODE_PRESET_node})
  set(_plus ${PP_NODE_PRESET_plus})
  list(SORT _sorted)
  list(SORT _node)
  list(SORT _plus)
  if(_sorted STREQUAL _node)
    set(_edition node)
  elseif(_sorted STREQUAL _plus)
    set(_edition plus)
  else()
    set(_edition custom)
  endif()
  set(${ARG_OUT_LIBS} "${_libs}" PARENT_SCOPE)
  set(${ARG_OUT_ALLOW} "${_allow}" PARENT_SCOPE)
  set(${ARG_OUT_REQUIRE} "${_require}" PARENT_SCOPE)
  set(${ARG_OUT_FORBID} "${_forbid}" PARENT_SCOPE)
  set(${ARG_OUT_EDITION} "${_edition}" PARENT_SCOPE)
endfunction()

# pp_add_node_binary(<target> <services> [EXCLUDE_FROM_ALL]): one node binary with that set, and its checks.
function(pp_add_node_binary target services)
  cmake_parse_arguments(ARG "EXCLUDE_FROM_ALL" "" "" ${ARGN})
  pp_node_services("${services}" OUT_LIBS _libs OUT_ALLOW _allow OUT_REQUIRE _require OUT_FORBID _forbid
    OUT_EDITION _edition)
  if(ARG_EXCLUDE_FROM_ALL)
    add_executable(${target} EXCLUDE_FROM_ALL ${CMAKE_CURRENT_SOURCE_DIR}/src/app/node/main.cpp)
  else()
    add_executable(${target} ${CMAKE_CURRENT_SOURCE_DIR}/src/app/node/main.cpp)
  endif()
  string(JOIN "," _names ${services})
  string(JOIN "," _known ${PP_NODE_KNOWN_SERVICES})
  target_compile_definitions(${target} PRIVATE
    "PP_NODE_EDITION=\"${_edition}\"" "PP_NODE_SERVICE_NAMES=\"${_names}\"" "PP_NODE_KNOWN_SERVICE_NAMES=\"${_known}\"")
  target_link_libraries(${target} PRIVATE pp_app_node ${_libs})
  pp_browser_apply_warnings(${target})
  # Fail configure / link if a PUBLIC_LIBS edge reintroduces SDL/RmlUi/GUI.
  pp_browser_assert_no_ui_dependencies(${target})
  # First-party closure stays foundation / common / lib {people,net} / headless domain, and this set's services.
  pp_browser_assert_node_link_allowlist(${target} EXTRA ${_allow})
  pp_browser_add_no_ui_link_check(${target})
  pp_browser_add_symbol_check(${target} FORBID ${_forbid} REQUIRE ${_require}
    WHY "A node binary links exactly its services (PP_NODE_SERVICES / PP_NODE_PLUS_SERVICES, projects/node-relay NR001).")
  message(STATUS "${target}: ${_edition} (services: ${_names})")
endfunction()
