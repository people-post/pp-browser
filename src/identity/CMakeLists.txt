set(PP_BROWSER_IDENTITY_SOURCES
  Ed25519Signer.cpp
  IdentityStore.cpp
)

add_library(pp_identity STATIC ${PP_BROWSER_IDENTITY_SOURCES})
target_include_directories(pp_identity PUBLIC
  ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(pp_identity PUBLIC
  pp_common
)
target_link_libraries(pp_identity PRIVATE
  nlohmann_json::nlohmann_json
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_identity PUBLIC OpenSSL::Crypto)
endif()