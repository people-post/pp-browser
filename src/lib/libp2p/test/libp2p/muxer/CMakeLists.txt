#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_subdirectory(yamux)

addtest(muxers_and_streams_test muxers_and_streams_test.cpp)

target_link_libraries(muxers_and_streams_test
    p2p_default_host
    )
