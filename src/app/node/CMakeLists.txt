if(PP_BROWSER_IS_MOBILE)
  return()
endif()

add_executable(pp-node
  main.cpp
  NodeBootstrap.cpp
)

target_include_directories(pp-node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/libp2p/fork/include
)

target_link_libraries(pp-node PRIVATE
  pp_libp2p_integration
  pp_base_people
  pp_base_crypto
  pp_base_data
  pp_base_platform_core
  pp_base_runtime_core
  pp_base_error
  pp_common
  nlohmann_json::nlohmann_json
  Threads::Threads
  sodium
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp-node PRIVATE OpenSSL::Crypto)
endif()

install(TARGETS pp-node RUNTIME DESTINATION bin)
