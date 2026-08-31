# Vendored libp2p / shared networking dependencies for pp-browser (replaces Hunter).
# Included from cmake/dependencies.cmake.
# A017: PeerId + wire only — no lsquic / c-ares / hat-trie.

include(Progress)

set(PP_LIBP2P_THIRD_PARTY "${PP_THIRD_PARTY_DIR}")

function(pp_libp2p_require_vendored name)
  if(NOT EXISTS "${PP_LIBP2P_THIRD_PARTY}/${name}/CMakeLists.txt")
    message(FATAL_ERROR
      "Missing libp2p vendored dependency '${name}' under third_party/.\n"
      "  Run: ./scripts/libp2p_vendor_import.sh")
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

function(pp_browser_add_vendored_boringssl)
  if(TARGET crypto AND TARGET ssl)
    if(NOT TARGET OpenSSL::Crypto)
      add_library(OpenSSL::Crypto ALIAS crypto)
    endif()
    if(NOT TARGET OpenSSL::SSL)
      add_library(OpenSSL::SSL ALIAS ssl)
    endif()
    return()
  endif()

  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  pp_libp2p_add_vendored(boringssl)
  if(NOT TARGET OpenSSL::Crypto)
    if(TARGET crypto)
      add_library(OpenSSL::Crypto ALIAS crypto)
    else()
      message(FATAL_ERROR "BoringSSL crypto target not found")
    endif()
  endif()
  if(NOT TARGET OpenSSL::SSL)
    if(TARGET ssl)
      add_library(OpenSSL::SSL ALIAS ssl)
    else()
      message(FATAL_ERROR "BoringSSL ssl target not found")
    endif()
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

# --- BoringSSL (curl TLS on Linux + PeerId SHA) ---
pp_browser_add_vendored_boringssl()

# --- standalone Asio (pp-node StatusHttpServer) ---
pp_libp2p_add_vendored(asio)
if(NOT TARGET Asio::asio)
  message(FATAL_ERROR "Asio::asio target not found")
endif()

# --- fmt ---
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(fmt)
pp_libp2p_alias(fmt fmt::fmt)

# --- yaml-cpp (soralog) ---
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(yaml-cpp)
pp_libp2p_alias(yaml-cpp yaml-cpp::yaml-cpp)

# --- standalone Outcome (before qtils) ---
pp_libp2p_add_vendored(outcome)
if(NOT TARGET Outcome::outcome)
  message(FATAL_ERROR "Outcome::outcome target not found")
endif()

# --- qtils ---
set(QTILS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FORMAT_ERROR_WITH_FULLTYPE ON CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(qtils)
pp_libp2p_alias(qtils qtils::qtils)

# --- soralog ---
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLANG_FORMAT OFF CACHE BOOL "" FORCE)
set(CLANG_TIDY OFF CACHE BOOL "" FORCE)
set(CMAKE_CXX_STANDARD 20)
pp_libp2p_add_vendored(soralog)
pp_libp2p_alias(soralog soralog::soralog)

pp_configure_status("libp2p third_party dependencies ready")
