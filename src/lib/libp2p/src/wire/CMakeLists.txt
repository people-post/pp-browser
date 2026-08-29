#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_wire
    wire_codec.cpp
    keys_wire.cpp
    noise_wire.cpp
    identify_wire.cpp
    plaintext_wire.cpp
    secio_wire.cpp
    kademlia_wire.cpp
    gossip_wire.cpp
    )
target_link_libraries(p2p_wire
    )
