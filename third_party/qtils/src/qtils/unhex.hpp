/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <string_view>

#include <qtils/bytes.hpp>
#include <qtils/enum_error_code.hpp>
#include <qtils/outcome.hpp>

constexpr size_t MAX_UNHEX_SIZE = 64 * 1024 * 1024;

namespace qtils {
  enum class UnhexError {
    UNEXPECTED_0X,
    REQUIRED_0X,
    ODD_LENGTH,
    TOO_SHORT,
    TOO_LONG,
    FIXED_SIZE_TOO_LONG,
    NON_HEX,
  };
  Q_ENUM_ERROR_CODE(UnhexError) {
    using E = decltype(e);
    switch (e) {
      case E::UNEXPECTED_0X:
        return "UNEXPECTED_0X";
      case E::REQUIRED_0X:
        return "REQUIRED_0X";
      case E::ODD_LENGTH:
        return "ODD_LENGTH";
      case E::TOO_SHORT:
        return "TOO_SHORT";
      case E::TOO_LONG:
        return "TOO_LONG";
      case E::FIXED_SIZE_TOO_LONG:
        return "FIXED_SIZE_TOO_LONG";
      case E::NON_HEX:
        return "NON_HEX";
    }
    abort();
  }

  inline size_t unhexSize(std::string_view s) {
    if (s.starts_with("0x")) {
      s.remove_prefix(2);
    }
    return s.size() / 2;
  }

  inline int unhexNibble(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  template <typename T>
  outcome::result<void> unhex(
      T &t, std::string_view s, size_t max_size = MAX_UNHEX_SIZE) {
    if (s.starts_with("0x")) {
      return UnhexError::UNEXPECTED_0X;
    }
    if (s.size() % 2 != 0) {
      return UnhexError::ODD_LENGTH;
    }
    const auto count = unhexSize(s);
    if constexpr (requires { t.resize(size_t{}); }) {
      if (count > max_size) {
        return UnhexError::FIXED_SIZE_TOO_LONG;
      }
      try {
        t.resize(count);
      } catch (const std::bad_alloc &) {
        return UnhexError::TOO_LONG;
      }
    } else {
      if (count < t.size()) {
        return UnhexError::TOO_SHORT;
      }
      if (count > t.size()) {
        return UnhexError::TOO_LONG;
      }
    }
    for (size_t i = 0; i < s.size(); i += 2) {
      const int hi = unhexNibble(s[i]);
      const int lo = unhexNibble(s[i + 1]);
      if (hi < 0 || lo < 0) {
        return UnhexError::NON_HEX;
      }
      t[i / 2] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return outcome::success();
  }

  template <typename T = Bytes>
  outcome::result<T> unhex(std::string_view s) {
    T t;
    OUTCOME_TRY(unhex(t, s));
    return t;
  }

  template <typename T>
  outcome::result<void> unhex0x(
      T &t, std::string_view s, bool optional_0x = false) {
    if (s.starts_with("0x")) {
      s.remove_prefix(2);
    } else if (not optional_0x) {
      return UnhexError::REQUIRED_0X;
    }
    return unhex<T>(t, s);
  }

  template <typename T = Bytes>
  outcome::result<T> unhex0x(std::string_view s) {
    T t;
    OUTCOME_TRY(unhex0x(t, s));
    return t;
  }

  inline auto operator""_unhex(const char *c, size_t s) {
    return unhex(std::string_view{c, s}).value();
  }
}  // namespace qtils
