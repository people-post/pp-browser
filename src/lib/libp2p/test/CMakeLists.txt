#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PROJECT_SOURCE_DIR})

if(PACKAGE_MANAGER STREQUAL "vendored")
  # Acceptance/helper test targets use add_library directly; ensure vendored includes resolve.
  link_libraries(qtils::qtils soralog::soralog GTest::gmock libsecp256k1::secp256k1)
endif()

add_subdirectory(acceptance)
add_subdirectory(libp2p)
add_subdirectory(testutil)
