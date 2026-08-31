#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_peer_id
    peer_id.cpp
    )
target_link_libraries(p2p_peer_id
    p2p_multihash
    p2p_multibase_codec
    p2p_sha
    )
