#
# Copyright Quadrivium LLC
# All Rights Reserved
# SPDX-License-Identifier: Apache-2.0
#

# A017: vendored PeerId/wire deps only (no Hunter / Host / QUIC).
macro(pp_libp2p_require_target target)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "Vendored libp2p dependency target missing: ${target}")
  endif()
endmacro()

if(NOT PACKAGE_MANAGER STREQUAL "vendored")
  message(FATAL_ERROR "libp2p fork requires PACKAGE_MANAGER=vendored (A017)")
endif()

pp_libp2p_require_target(OpenSSL::Crypto)
pp_libp2p_require_target(OpenSSL::SSL)
pp_libp2p_require_target(qtils::qtils)
find_package(Threads REQUIRED)
pp_libp2p_require_target(fmt::fmt)
pp_libp2p_require_target(yaml-cpp::yaml-cpp)
pp_libp2p_require_target(soralog::soralog)
