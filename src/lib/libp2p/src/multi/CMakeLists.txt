#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_subdirectory(multibase_codec)

libp2p_add_library(p2p_uvarint
    uvarint.cpp
    )
target_link_libraries(p2p_uvarint
    p2p_logger
    )

libp2p_add_library(p2p_multihash
    multihash.cpp
    )
target_link_libraries(p2p_multihash
    p2p_varint_prefix_reader
    )
