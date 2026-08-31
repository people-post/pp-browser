#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_varint_prefix_reader
    varint_prefix_reader.cpp
    )
target_link_libraries(p2p_varint_prefix_reader
    p2p_logger
    )
