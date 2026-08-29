#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#



libp2p_add_library(p2p_transport_parser
    multiaddress_parser.cpp
    )
target_link_libraries(p2p_transport_parser
    p2p_multiaddress
    )

libp2p_add_library(p2p_upgrader
    upgrader_impl.cpp
    )
target_link_libraries(p2p_upgrader
    )


libp2p_add_library(p2p_upgrader_session
    upgrader_session.cpp
    )
target_link_libraries(p2p_upgrader_session
    p2p_upgrader
    )
