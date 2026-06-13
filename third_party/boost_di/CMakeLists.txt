#
# Copyright (c) 2012-2019 Kris Jusiak (kris at jusiak dot net)
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
#
cmake_minimum_required(VERSION 2.8)
cmake_policy(SET CMP0048 NEW)
project(Boost.DI VERSION 1.1.0 LANGUAGES CXX)
set(CXX_STANDARD 14 CACHE STRING "C++ Standard Version. [14|17|20]")

add_library(${PROJECT_NAME} INTERFACE)
target_include_directories(
    ${PROJECT_NAME} INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/extension/include>
)

if (${CMAKE_CXX_COMPILER_ID} STREQUAL GNU)
  set(COMPILER_SPECIFIC_CXX_STANDARD "-std=c++${CXX_STANDARD}")
  string(REPLACE "14" "1y" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})
  string(REPLACE "17" "1z" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})
  string(REPLACE "20" "2a" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_SPECIFIC_CXX_STANDARD} -fno-exceptions -pedantic -pedantic-errors -Wall -Wextra -Werror")

elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL Clang OR ${CMAKE_CXX_COMPILER_ID} STREQUAL AppleClang)
  set(COMPILER_SPECIFIC_CXX_STANDARD "-std=c++${CXX_STANDARD}")
  string(REPLACE "14" "1y" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})
  string(REPLACE "17" "1z" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})
  string(REPLACE "20" "2a" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_SPECIFIC_CXX_STANDARD} -fno-exceptions -pedantic -pedantic-errors -Wall -Wextra -Werror")

elseif (${CMAKE_CXX_COMPILER_ID} STREQUAL MSVC)
  set(COMPILER_SPECIFIC_CXX_STANDARD "/std:c++${CXX_STANDARD}")
  string(REPLACE "20" "latest" COMPILER_SPECIFIC_CXX_STANDARD ${COMPILER_SPECIFIC_CXX_STANDARD})

  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${COMPILER_SPECIFIC_CXX_STANDARD} /W3 /EHsc /Zc:__cplusplus")

else ()
  message(WARNING "Unsupported compiler!")
endif ()

if (CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  set(IS_TOPLEVEL_PROJECT TRUE)
else ()
  set(IS_TOPLEVEL_PROJECT FALSE)
endif ()

#option(TESTS_DISABLED "Tests are disabled." ${BOOST_DI_DISABLE_TESTS})
option(BOOST_DI_OPT_BUILD_TESTS "Build and perform Boost.DI tests" ${IS_TOPLEVEL_PROJECT})
if (BOOST_DI_OPT_BUILD_TESTS)
  message("Boost::di: Include tests.")
  enable_testing()
  add_subdirectory(extension/test)
  add_subdirectory(test)
else ()
  message("Boost::di: Tests are not included.")
endif ()

option(BOOST_DI_OPT_BUILD_EXAMPLES "Build perform Boost.DI examples" ${IS_TOPLEVEL_PROJECT})
if (BOOST_DI_OPT_BUILD_EXAMPLES)
  message("Boost::di: Include examples.")
  add_subdirectory(example)
else ()
  message("Boost::di: Examples are not included.")
endif ()

include(GNUInstallDirs)

function(install_target targets config)
  install(TARGETS ${targets} EXPORT ${config}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
      PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
      FRAMEWORK DESTINATION ${CMAKE_INSTALL_PREFIX}
      )
endfunction()

set(CONFIG_NAME "${PROJECT_NAME}Config")
install_target(${PROJECT_NAME} ${CONFIG_NAME})

install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/extension/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

set(CONFIG_DESTINATION_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}")
install(
    EXPORT ${CONFIG_NAME}
    NAMESPACE Boost::
    DESTINATION ${CONFIG_DESTINATION_DIR}
)
