# Standalone Outcome (ned14) — single-header amalgamation.
# https://github.com/ned14/outcome  (tag v2.2.15, single-header/outcome.hpp)

cmake_minimum_required(VERSION 3.12)
project(outcome LANGUAGES CXX)

add_library(outcome INTERFACE)
add_library(Outcome::outcome ALIAS outcome)

target_include_directories(outcome INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:include>
)

target_compile_features(outcome INTERFACE cxx_std_17)
