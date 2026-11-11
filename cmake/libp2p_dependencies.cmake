# Vendored libp2p / shared networking dependencies for pp-browser (replaces Hunter).
# Included from cmake/dependencies.cmake.
# A017: PeerId + wire only — no lsquic / c-ares / hat-trie.

include(Progress)

set(PP_LIBP2P_THIRD_PARTY "${PP_THIRD_PARTY_DIR}")

function(pp_libp2p_require_vendored name)
  if(NOT EXISTS "${PP_LIBP2P_THIRD_PARTY}/${name}/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing libp2p vendored dependency '${name}' under third_party/.\n"
      "  Run: ./scripts/vendor/libp2p_vendor_import.sh")
  endif()
endfunction()

function(pp_libp2p_add_vendored name)
  pp_libp2p_require_vendored(${name})
  add_subdirectory(
    "${PP_LIBP2P_THIRD_PARTY}/${name}"
    "${CMAKE_BINARY_DIR}/third_party/${name}"
    EXCLUDE_FROM_ALL)
endfunction()

function(pp_libp2p_alias target alias)
  if(TARGET ${target} AND NOT TARGET ${alias})
    add_library(${alias} ALIAS ${target})
  endif()
endfunction()

# BoringSSL comes from pp-cpp-crypto (the link AEAD's engine); curl's FindOpenSSL takes these aliases.
function(pp_browser_alias_boringssl)
  if(NOT TARGET crypto OR NOT TARGET ssl OR NOT PP_CRYPTO_BORINGSSL_INCLUDE_DIR)
    message(FATAL_ERROR "pp-cpp-crypto did not define BoringSSL (targets crypto / ssl); include PpCppCrypto first")
  endif()
  if(NOT TARGET OpenSSL::Crypto)
    add_library(OpenSSL::Crypto ALIAS crypto)
  endif()
  if(NOT TARGET OpenSSL::SSL)
    add_library(OpenSSL::SSL ALIAS ssl)
  endif()
endfunction()

pp_configure_status("Configuring libp2p third_party dependencies...")

# Silence cmake_minimum_required deprecation warnings from older vendored deps on CMake 3.29+.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.29")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

set(PACKAGE_MANAGER vendored CACHE STRING "Dependency manager for qdrvm libs" FORCE)

# --- zlib (curl / FreeType reuse) ---
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(zlib)
if(TARGET zlib)
  target_include_directories(zlib PUBLIC
    "$<BUILD_INTERFACE:${PP_LIBP2P_THIRD_PARTY}/zlib>"
    "$<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/third_party/zlib>")
endif()
if(NOT TARGET ZLIB::ZLIB)
  if(TARGET ZLIB::zlib)
    add_library(ZLIB::ZLIB ALIAS ZLIB::zlib)
  elseif(TARGET zlibstatic)
    add_library(ZLIB::ZLIB ALIAS zlibstatic)
  elseif(TARGET zlib)
    add_library(ZLIB::ZLIB ALIAS zlib)
  else()
    message(FATAL_ERROR "ZLIB target not found after vendoring zlib")
  endif()
endif()

# --- BoringSSL (pp-cpp-crypto's; curl's TLS on Linux, Android and iOS; Windows uses Schannel, macOS SecureTransport) ---
pp_browser_alias_boringssl()

# --- standalone Asio (pp-node StatusHttpServer) ---
pp_libp2p_add_vendored(asio)
if(NOT TARGET Asio::asio)
  message(FATAL_ERROR "Asio::asio target not found")
endif()

pp_configure_status("libp2p third_party dependencies ready")
