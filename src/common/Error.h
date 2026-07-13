#pragma once

#include "common/ResultOrError.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace pbr {

/** Low-level error payload. category/code are opaque int32_t; interpret at higher layers. */
struct Error : public RoeErrorBase {
  Error() : RoeErrorBase() {}
  Error(int32_t c, const std::string& msg) : RoeErrorBase(c, msg) {}
  Error(int32_t c, std::string&& msg) : RoeErrorBase(c, std::move(msg)) {}
  explicit Error(const std::string& msg) : RoeErrorBase(msg) {}
  explicit Error(std::string&& msg) : RoeErrorBase(std::move(msg)) {}

  Error& WithUser(const std::string& text) & {
    user = text;
    return *this;
  }
  Error&& WithUser(const std::string& text) && {
    user = text;
    return std::move(*this);
  }

  static Error Make(int32_t category, int32_t code, const std::string& detail) {
    Error err;
    err.category = category;
    err.code = code;
    err.message = detail;
    return err;
  }
};

template <typename T>
using Roe = ResultOrError<T, Error>;

} // namespace pbr
