#pragma once

#include "common/Error.h"
#include "common/Serialize.hpp"

#include <sstream>
#include <string>

namespace pbr {

/**
 * Pack a struct/object to binary using OutputArchive (pp Binary Wire Profile).
 */
template <typename T> std::string binaryPack(const T &t) {
  std::ostringstream oss;
  OutputArchive ar(oss);
  ar & t;
  return oss.str();
}

/**
 * Unpack binary bytes into T. Fails on parse error or trailing bytes.
 */
template <typename T> Roe<T> binaryUnpack(const std::string &data) {
  std::istringstream iss(data);
  InputArchive ar(iss);
  T result;
  ar & result;
  if (ar.failed()) {
    return Error("Failed to deserialize binary data");
  }
  if (!ar.exactEnd()) {
    return Error("Trailing bytes after binary decode");
  }
  return result;
}

} // namespace pbr
