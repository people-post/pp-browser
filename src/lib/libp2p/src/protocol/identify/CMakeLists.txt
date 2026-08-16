#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_identify
    identify.cpp
    identify_msg_processor.cpp
    observed_addresses.cpp
    identify_push.cpp
    identify_delta.cpp
    utils.cpp
    )
target_link_libraries(p2p_identify
    p2p
    p2p_wire
    p2p_protobuf_message_read_writer
    p2p_logger
    )
