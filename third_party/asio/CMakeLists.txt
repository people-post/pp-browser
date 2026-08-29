# Standalone Asio (chriskohlhoff) — header-only.
# https://github.com/chriskohlhoff/asio  (tag asio-1-34-0)

cmake_minimum_required(VERSION 3.12)
project(asio LANGUAGES CXX)

find_package(Threads REQUIRED)

add_library(asio INTERFACE)
add_library(Asio::asio ALIAS asio)

target_include_directories(asio INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(asio INTERFACE cxx_std_17)

target_compile_definitions(asio INTERFACE
  ASIO_STANDALONE
  ASIO_NO_DEPRECATED
)

target_link_libraries(asio INTERFACE Threads::Threads)
