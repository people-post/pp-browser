#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

add_subdirectory(aes_provider)
add_subdirectory(chachapoly)
add_subdirectory(crypto_provider)
add_subdirectory(ecdsa_provider)
add_subdirectory(ed25519_provider)
add_subdirectory(mldsa_provider)
add_subdirectory(hmac_provider)
add_subdirectory(key_marshaller)
add_subdirectory(key_validator)
add_subdirectory(random_generator)
add_subdirectory(rsa_provider)
add_subdirectory(sha)
add_subdirectory(x25519_provider)

libp2p_add_library(p2p_crypto_error
    error.cpp
    )
target_link_libraries(p2p_crypto_error
    )

libp2p_add_library(p2p_crypto_key
    key.cpp
    )
target_link_libraries(p2p_crypto_key
    )

libp2p_add_library(p2p_crypto_common
    common_functions.cpp
    )
target_link_libraries(p2p_crypto_common
    p2p_crypto_error
    OpenSSL::Crypto
    )
