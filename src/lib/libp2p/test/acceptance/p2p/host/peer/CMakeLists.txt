#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_library(p2p_test_peer
    test_peer.hpp
    test_peer.cpp
    )
target_link_libraries(p2p_test_peer
    tsl::tsl_hat_trie
    p2p_default_host
    p2p_protocol_echo
    p2p_client_test_session
    )
