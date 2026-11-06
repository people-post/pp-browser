#pragma once

#include <filesystem>
#include <string>

namespace pbr {

/** A path from UTF-8 bytes. `std::filesystem::path(std::string)` reads narrow text as the ANSI code page on Windows,
 *  so a CJK or emoji file name throws or turns to garbage there; the char8_t overload is UTF-8 on every platform. */
inline std::filesystem::path PathFromUtf8(const std::string& utf8) {
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

/** The path as UTF-8 bytes (the inverse of PathFromUtf8). */
inline std::string PathToUtf8(const std::filesystem::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

} // namespace pbr
