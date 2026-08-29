#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

libp2p_add_library(p2p_router
    router_impl.cpp
    )
target_link_libraries(p2p_router
    tsl::tsl_hat_trie
    p2p_peer_id
    )


libp2p_add_library(p2p_listener_manager
    listener_manager_impl.cpp
    )
target_link_libraries(p2p_listener_manager
    p2p_multiaddress
    p2p_peer_id
    p2p_logger
    )


libp2p_add_library(p2p_dialer
    dialer_impl.cpp
    )
target_link_libraries(p2p_dialer
    p2p_multiaddress
    p2p_multiselect
    p2p_peer_id
    p2p_logger
    )


libp2p_add_library(p2p_network
    network_impl.cpp
    )
target_link_libraries(p2p_network
    )

libp2p_add_library(p2p_transport_manager
    transport_manager_impl.cpp
    )
target_link_libraries(p2p_transport_manager
    p2p_multiaddress
    )

libp2p_add_library(p2p_connection_manager
    connection_manager_impl.cpp
    )
target_link_libraries(p2p_connection_manager
    )

libp2p_add_library(p2p_dnsaddr_resolver
    dnsaddr_resolver_impl.cpp
    )
target_link_libraries(p2p_dnsaddr_resolver
    p2p_cares
    )
