#pragma once

#include "common/ResultOrError.hpp"

namespace pbr {

struct Error : public RoeErrorBase {
  Error() : RoeErrorBase() {}
  Error(int32_t c, const std::string& msg) : RoeErrorBase(c, msg) {}
  Error(int32_t c, std::string&& msg) : RoeErrorBase(c, std::move(msg)) {}
  explicit Error(const std::string& msg) : RoeErrorBase(msg) {}
  explicit Error(std::string&& msg) : RoeErrorBase(std::move(msg)) {}
};

template <typename T>
using Roe = ResultOrError<T, Error>;

} // namespace pbr
