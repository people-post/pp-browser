/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/multi/converters/ip_v4_converter.hpp>

#include <asio/ip/address_v4.hpp>
#include <system_error>
#include <cctype>
#include <libp2p/multi/converters/conversion_error.hpp>

namespace libp2p::multi::converters {
  namespace {
    // Windows inet_pton accepts shorthand forms like "127.0.0"; multiaddr
    // requires canonical dotted-decimal with exactly four octets.
    bool isStrictDottedDecimalIpv4(std::string_view addr) {
      if (addr.empty() || addr.back() == '.') {
        return false;
      }

      size_t octets = 0;
      for (size_t i = 0; i < addr.size();) {
        if (octets == 4 || !std::isdigit(static_cast<unsigned char>(addr[i]))) {
          return false;
        }

        uint32_t value = 0;
        const auto start = i;
        while (i < addr.size()
               && std::isdigit(static_cast<unsigned char>(addr[i]))) {
          value = value * 10 + static_cast<uint32_t>(addr[i] - '0');
          if (value > 255) {
            return false;
          }
          ++i;
        }
        if (i == start) {
          return false;
        }
        ++octets;

        if (i == addr.size()) {
          break;
        }
        if (addr[i] != '.') {
          return false;
        }
        ++i;
      }

      return octets == 4;
    }
  }  // namespace

  outcome::result<Bytes> IPv4Converter::addressToBytes(std::string_view addr) {
    if (!isStrictDottedDecimalIpv4(addr)) {
      return ConversionError::INVALID_ADDRESS;
    }

    std::error_code ec;
    auto address = asio::ip::make_address_v4(addr, ec);
    if (ec) {
      return ConversionError::INVALID_ADDRESS;
    }
    auto ip_bytes = address.to_bytes();
    return Bytes(ip_bytes.begin(), ip_bytes.end());
  }

}  // namespace libp2p::multi::converters
