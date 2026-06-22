#
# Copyright Soramitsu Co., 2021-2023
# Copyright Quadrivium Co., 2023
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

set(SORALOG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set(SORALOG_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

set(INSTALL_TARGETS)
function (add_for_install target)
    list(APPEND INSTALL_TARGETS ${target})
    set(INSTALL_TARGETS ${INSTALL_TARGETS} PARENT_SCOPE)
endfunction()

add_library(sink INTERFACE)
target_link_libraries(sink INTERFACE
    fmt::fmt
    )

add_library(sink_to_nowhere
    impl/sink_to_nowhere.cpp
    )
target_link_libraries(sink_to_nowhere
    sink
    )

add_library(sink_to_console
    impl/sink_to_console.cpp
    )
target_link_libraries(sink_to_console
    sink
    )
if(NOT WIN32 AND NOT ANDROID)
  target_link_libraries(sink_to_console pthread)
endif()

add_library(sink_to_file
    impl/sink_to_file.cpp
    )
target_link_libraries(sink_to_file
    sink
    )
if(NOT WIN32 AND NOT ANDROID)
  target_link_libraries(sink_to_file pthread)
endif()

if(NOT WIN32 AND NOT ANDROID)
add_library(sink_to_syslog
    impl/sink_to_syslog.cpp
    )
target_link_libraries(sink_to_syslog
    sink
    pthread
    )
endif()

add_library(multisink
    impl/multisink.cpp
    )
target_link_libraries(multisink
    sink
    )

add_library(group
    group.cpp
    )

add_library(logger
    logger.cpp
    )
target_link_libraries(logger
    sink
    )

add_library(configurator INTERFACE)
target_link_libraries(configurator INTERFACE
    sink_to_nowhere
    sink_to_console
    sink_to_file
    )
if(NOT WIN32 AND NOT ANDROID)
  target_link_libraries(configurator INTERFACE sink_to_syslog)
endif()
target_link_libraries(configurator INTERFACE
    multisink
    )

add_library(configurator_yaml
    impl/configurator_from_yaml.cpp
    )
target_link_libraries(configurator_yaml
    yaml-cpp::yaml-cpp
    configurator
    logging_system
    )

add_library(fallback_configurator
    impl/fallback_configurator.cpp
    )
target_link_libraries(fallback_configurator
    configurator
    )

add_library(logging_system
    logging_system.cpp
    )
target_link_libraries(logging_system
    group
    logger
    sink
    sink_to_nowhere
    )

add_library(soralog soralog.cpp)
target_link_libraries(soralog
    logging_system
    sink
    group
    configurator
    logger
    )

add_library(yaml ALIAS configurator_yaml)
add_library(fallback ALIAS fallback_configurator)

add_library(soralog::soralog ALIAS soralog)
add_library(soralog::yaml ALIAS configurator_yaml)
add_library(soralog::fallback ALIAS fallback_configurator)
add_library(soralog::fallback_configurator ALIAS fallback_configurator)
add_library(soralog::configurator_yaml ALIAS configurator_yaml)

set(INSTALL_TARGETS
    sink
    sink_to_nowhere
    sink_to_console
    sink_to_file
    )
if(NOT WIN32 AND NOT ANDROID)
  list(APPEND INSTALL_TARGETS sink_to_syslog)
endif()
list(APPEND INSTALL_TARGETS
    multisink

    group

    configurator
    fallback_configurator
    configurator_yaml

    logger
    logging_system

    soralog
    )

foreach(_soralog_target ${INSTALL_TARGETS})
  get_target_property(_soralog_type ${_soralog_target} TYPE)
  if(_soralog_type STREQUAL "INTERFACE_LIBRARY")
    target_include_directories(${_soralog_target} SYSTEM INTERFACE
      $<BUILD_INTERFACE:${SORALOG_INCLUDE_DIR}>
      $<BUILD_INTERFACE:${SORALOG_SRC_DIR}>
    )
  else()
    target_include_directories(${_soralog_target} SYSTEM PUBLIC
      $<BUILD_INTERFACE:${SORALOG_INCLUDE_DIR}>
      $<BUILD_INTERFACE:${SORALOG_SRC_DIR}>
    )
  endif()
endforeach()

include(GNUInstallDirs)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
install(
    DIRECTORY ${CMAKE_SOURCE_DIR}/include/soralog
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(
    TARGETS ${INSTALL_TARGETS}
    EXPORT soralogConfig
    LIBRARY       DESTINATION ${CMAKE_INSTALL_LIBDIR}/soralog
    ARCHIVE       DESTINATION ${CMAKE_INSTALL_LIBDIR}/soralog
    RUNTIME       DESTINATION ${CMAKE_INSTALL_BINDIR}/soralog
    INCLUDES      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FRAMEWORK     DESTINATION ${CMAKE_INSTALL_PREFIX}
)

install(
    EXPORT soralogConfig
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/soralog
    NAMESPACE soralog::
)

export(
    EXPORT soralogConfig
    FILE ${PROJECT_BINARY_DIR}/soralogConfig.cmake
    NAMESPACE soralog::
)
endif()
