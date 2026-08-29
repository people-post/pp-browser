cmake_minimum_required(VERSION 3.12)
project(Boost VERSION 1.87.0 LANGUAGES CXX)

set(BOOST_SUPERPROJECT_VERSION "1.87.0")
set(BOOST_SUPERPROJECT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(Boost_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(Boost_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/include")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BOOST_SKIP_INSTALL_RULES ON CACHE BOOL "" FORCE)
# Minimal Boost for Boost.DI (optional boost::shared_ptr via smart_ptr) and
# header packaging. Standalone Asio + std::error_code replaced Boost.Asio/System.
set(BOOST_INCLUDE_LIBRARIES
    headers
    assert
    config
    throw_exception
    variant2
    winapi
    predef
    type_traits
    static_assert
    core
    smart_ptr
    move
    utility
    CACHE STRING "" FORCE)

include(BoostRoot)

# Headers for predef/winapi live in the unified boost/ tree (libs/predef/include is absent).
if(TARGET boost_predef)
  target_include_directories(boost_predef INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# Compiled Boost libs include headers from the unified boost/ tree at the superproject root.
foreach(_boost_compiled_target boost_atomic)
  if(TARGET ${_boost_compiled_target})
    target_include_directories(${_boost_compiled_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
endforeach()

if(NOT TARGET Boost::boost)
  add_library(Boost_boost INTERFACE)
  add_library(Boost::boost ALIAS Boost_boost)
  target_include_directories(Boost_boost INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(Boost_boost INTERFACE Boost::headers)
  # Vendored Boost is linked via CMake targets; disable MSVC #pragma comment(lib) auto-link.
  target_compile_definitions(Boost_boost INTERFACE BOOST_ALL_NO_LIB)
endif()
