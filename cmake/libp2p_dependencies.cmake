# Vendored libp2p dependencies for pp-browser (replaces Hunter).
# Included from cmake/dependencies.cmake.

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

# --- zlib (before lsquic) ---
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

# --- BoringSSL (before curl on Linux and before lsquic) ---
pp_browser_add_vendored_boringssl()

# --- Boost ---
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(boost)

# --- fmt ---
set(FMT_DOC OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(fmt)
pp_libp2p_alias(fmt fmt::fmt)

# --- yaml-cpp ---
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(yaml-cpp)
pp_libp2p_alias(yaml-cpp yaml-cpp::yaml-cpp)

# --- c-ares ---
set(CARES_STATIC ON CACHE BOOL "" FORCE)
set(CARES_SHARED OFF CACHE BOOL "" FORCE)
set(CARES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CARES_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(c-ares)
if(NOT TARGET c-ares::cares)
  if(TARGET c-ares::cares_static)
    add_library(c-ares::cares ALIAS c-ares::cares_static)
  elseif(TARGET cares)
    add_library(c-ares::cares ALIAS cares)
  else()
    message(FATAL_ERROR "c-ares target not found")
  endif()
endif()

# --- libsecp256k1 ---
set(SECP256K1_ENABLE_MODULE_ECDH OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(libsecp256k1)
pp_libp2p_alias(secp256k1 libsecp256k1::secp256k1)

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

# --- tsl_hat_trie ---
pp_libp2p_add_vendored(tsl_hat_trie)
if(NOT TARGET tsl::tsl_hat_trie)
  if(TARGET tsl::hat_trie)
    add_library(tsl::tsl_hat_trie ALIAS tsl_hat_trie)
  elseif(TARGET tsl_hat_trie)
    add_library(tsl::tsl_hat_trie ALIAS tsl_hat_trie)
  else()
    message(FATAL_ERROR "tsl_hat_trie target not found")
  endif()
endif()

# --- Boost.DI ---
set(BOOST_DI_OPT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BOOST_DI_OPT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
pp_libp2p_add_vendored(boost_di)
if(NOT TARGET Boost::Boost.DI)
  if(TARGET Boost.DI)
    add_library(Boost::Boost.DI ALIAS Boost.DI)
  else()
    message(FATAL_ERROR "Boost.DI target not found")
  endif()
endif()

# --- lsquic ---
set(HUNTER_ENABLED OFF CACHE BOOL "" FORCE)
set(LSQUIC_BIN OFF CACHE BOOL "" FORCE)
set(LSQUIC_TESTS OFF CACHE BOOL "" FORCE)
set(LSQUIC_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(BORINGSSL_LIB_ssl ssl)
set(BORINGSSL_LIB_crypto crypto)
set(BORINGSSL_INCLUDE "${PP_LIBP2P_THIRD_PARTY}/boringssl/include")
set(ZLIB_INCLUDE "${PP_LIBP2P_THIRD_PARTY}/zlib")
set(ZLIB_INCLUDE_DIR "${ZLIB_INCLUDE}" CACHE PATH "" FORCE)
set(ZLIB_BINARY_INCLUDE "${CMAKE_BINARY_DIR}/third_party/zlib")
set(ZLIB_LIB ZLIB::ZLIB)
pp_libp2p_add_vendored(lsquic)
if(TARGET lsquic)
  target_include_directories(lsquic PUBLIC
    "$<BUILD_INTERFACE:${PP_LIBP2P_THIRD_PARTY}/lsquic/include>"
  )
  if(WIN32)
    target_include_directories(lsquic PUBLIC
      "$<BUILD_INTERFACE:${PP_LIBP2P_THIRD_PARTY}/lsquic/wincompat>"
    )
  endif()
  if(NOT HUNTER_ENABLED)
    target_link_libraries(lsquic PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
  endif()
endif()
if(NOT TARGET lsquic::lsquic)
  if(TARGET lsquic)
    add_library(lsquic::lsquic ALIAS lsquic)
  else()
    message(FATAL_ERROR "lsquic target not found")
  endif()
endif()

function(pp_libp2p_add_vendored_googletest)
  if(TARGET GTest::gmock_main)
    return()
  endif()
  pp_libp2p_require_vendored(googletest)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(gmock_force_shared_crt ON CACHE BOOL "" FORCE)
  set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_build_tests OFF CACHE BOOL "" FORCE)
  set(gtest_build_samples OFF CACHE BOOL "" FORCE)
  add_subdirectory(
    "${PP_LIBP2P_THIRD_PARTY}/googletest"
    "${CMAKE_BINARY_DIR}/third_party/googletest"
    EXCLUDE_FROM_ALL)
endfunction()

if(PP_BROWSER_LIBP2P_TESTING OR PP_BROWSER_LIBP2P_COVERAGE)
  pp_libp2p_add_vendored_googletest()
endif()

pp_configure_status("libp2p third_party dependencies ready")
