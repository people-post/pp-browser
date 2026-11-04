#pragma once

#include <sstream>
#include <string>
#include <vector>

namespace pbr {

/** Split whitespace-separated CLI-style args text. */
inline std::vector<std::string> ParseArgsText(const std::string& args_text) {
  std::vector<std::string> args;
  std::istringstream stream(args_text);
  std::string token;
  while (stream >> token) {
    args.push_back(token);
  }
  return args;
}

/** Join args with single spaces (inverse of ParseArgsText for simple tokens). */
inline std::string JoinArgsText(const std::vector<std::string>& args) {
  std::ostringstream out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i > 0) {
      out << ' ';
    }
    out << args[i];
  }
  return out.str();
}

} // namespace pbr
