#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_subdirectory(sha)

libp2p_add_library(p2p_crypto_error
    error.cpp
    )
target_link_libraries(p2p_crypto_error
    )
