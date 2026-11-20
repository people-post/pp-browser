#include "foundation/tls/RelayClientHeader.h"

#include <algorithm>
#include <cctype>
#include <mutex>
#include <utility>

namespace pbr {

namespace {

std::mutex& HeaderMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& HeaderValue() {
  static std::string value;
  return value;
}

bool SameName(const std::string& a, const std::string& b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

} // namespace

void SetRelayClientHeader(std::string value) {
  std::lock_guard lock(HeaderMutex());
  HeaderValue() = std::move(value);
}

std::string RelayClientHeader() {
  std::lock_guard lock(HeaderMutex());
  return HeaderValue();
}

std::map<std::string, std::string> WithRelayClientHeader(std::map<std::string, std::string> headers) {
  std::string value = RelayClientHeader();
  if (value.empty()) {
    return headers;
  }
  const std::string name = kRelayClientHeaderName;
  if (std::none_of(headers.begin(), headers.end(), [&](const auto& kv) { return SameName(kv.first, name); })) {
    headers.emplace(name, std::move(value));
  }
  return headers;
}

} // namespace pbr
