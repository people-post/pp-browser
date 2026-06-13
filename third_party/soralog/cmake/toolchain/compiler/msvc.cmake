#
# Copyright Soramitsu Co., 2021-2023
# Copyright Quadrivium Co., 2023
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

if(DEFINED POLLY_COMPILER_MSVC_CMAKE)
    return()
else()
    set(POLLY_COMPILER_MSVC_CMAKE 1)
endif()

set(
    MAX_SUPPORTED_CXX_STANDARD 20
    CACHE STRING "Max supported C++ standard"
    FORCE
)
