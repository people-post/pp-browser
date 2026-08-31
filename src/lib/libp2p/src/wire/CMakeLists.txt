#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_wire
    wire_codec.cpp
    keys_wire.cpp
    )
target_link_libraries(p2p_wire
    )
