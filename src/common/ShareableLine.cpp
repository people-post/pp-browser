#include "common/ShareableLine.h"

#include <sodium.h>

#include <array>
#include <cstdio>
#include <cstdlib>

namespace pbr {

namespace {

std::string_view WithoutScheme(std::string_view id) {
  for (const std::string_view scheme : {"account:", "peer:", "relay:", "call:"}) {
    if (id.substr(0, scheme.size()) == scheme) {
      id.remove_prefix(scheme.size());
      break;
    }
  }
  return id;
}

/** Random per process, in memory only. */
const std::array<unsigned char, crypto_generichash_KEYBYTES>& RefKey() {
  static const std::array<unsigned char, crypto_generichash_KEYBYTES> key = [] {
    std::array<unsigned char, crypto_generichash_KEYBYTES> k{};
    if (sodium_init() < 0) {
      std::abort();  // no randomness: never fall back to a guessable key
    }
    randombytes_buf(k.data(), k.size());
    return k;
  }();
  return key;
}

} // namespace

std::string ShareableRef(std::string_view id) {
  id = WithoutScheme(id);
  if (id.empty()) {
    return "-";
  }
  const auto& key = RefKey();
  std::array<unsigned char, 16> out{};
  crypto_generichash(out.data(), out.size(), reinterpret_cast<const unsigned char*>(id.data()), id.size(),
                     key.data(), key.size());
  char hex[9];
  std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", out[0], out[1], out[2], out[3]);
  return hex;
}

std::string ShortCallId(std::string_view call_id) {
  if (call_id.substr(0, 5) == "call:") {
    call_id.remove_prefix(5);
  }
  return std::string(call_id.substr(0, 8));
}

ShareableLine::ShareableLine(logging::Logger& sink, const ShareWord event) : sink_(&sink) {
  text_ = "event=";
  text_ += event.view();
}

ShareableLine& ShareableLine::AddRaw(const ShareWord key, const std::string_view value) {
  text_ += ' ';
  text_ += key.view();
  text_ += '=';
  text_ += value.empty() ? std::string_view("-") : value;
  return *this;
}

ShareableLine& ShareableLine::Add(const ShareWord key, const ShareWord value) { return AddRaw(key, value.view()); }

ShareableLine& ShareableLine::AddInt(const ShareWord key, const int64_t value) {
  return AddRaw(key, std::to_string(value));
}

ShareableLine& ShareableLine::AddUint(const ShareWord key, const uint64_t value) {
  return AddRaw(key, std::to_string(value));
}

ShareableLine& ShareableLine::AddDouble(const ShareWord key, const double value) {
  char buf[48];
  std::snprintf(buf, sizeof(buf), "%.1f", value);
  return AddRaw(key, buf);
}

ShareableLine& ShareableLine::Pick(const ShareWord key, const std::string_view value,
                                   const std::span<const ShareWord> words) {
  if (value.empty()) {
    return AddRaw(key, "-");
  }
  for (const ShareWord& word : words) {
    if (word.view() == value) {
      return AddRaw(key, word.view());
    }
  }
  return AddRaw(key, "other");
}

ShareableLine& ShareableLine::Ref(const ShareWord key, const std::string_view id) {
  return AddRaw(key, ShareableRef(id));
}

ShareableLine& ShareableLine::Call(const ShareWord key, const std::string_view call_id) {
  // Hex only: a call id is random hex, and anything else must not slip through as a "call id".
  std::string short_id = ShortCallId(call_id);
  for (const char c : short_id) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) {
      return AddRaw(key, "?");
    }
  }
  return AddRaw(key, short_id);
}

ShareableLine& ShareableLine::Fail(const ShareWord key, const Error& error) {
  return AddRaw(key, std::to_string(error.category) + "." + std::to_string(error.code));
}

void ShareableLine::Emit() const { sink_->info << text_; }

} // namespace pbr
