#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_executable(libp2p_echo_server
    libp2p_echo_server.cpp
    )
target_link_libraries(libp2p_echo_server
    p2p_default_host
    p2p_protocol_echo
    p2p_literals
    )

add_executable(libp2p_echo_client
    libp2p_echo_client.cpp
    )

target_link_libraries(libp2p_echo_client
    p2p_default_host
    p2p_asio_scheduler_backend
    p2p_basic_scheduler
    p2p_protocol_echo
    p2p_literals
    )
