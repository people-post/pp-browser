#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_executable(example_ares_resolver
    ares_resolver.cpp
    )
target_link_libraries(example_ares_resolver
    p2p_cares
    p2p_logger
    fmt::fmt
    )
