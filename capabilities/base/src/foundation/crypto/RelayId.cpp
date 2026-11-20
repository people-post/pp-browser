#include "foundation/crypto/RelayId.h"

#include "foundation/crypto/CryptoUtil.h"

#include <algorithm>
#include <cstring>

namespace pbr {

namespace {

constexpr size_t kPrefixLength = sizeof(kRelayIdPrefix) - 1;

} // namespace

std::optional<RelayId> RelayId::FromRaw(const std::span<const uint8_t> raw) {
  if (raw.size() != kRelayIdBytes) {
    return std::nullopt;
  }
  RelayId id;
  std::copy(raw.begin(), raw.end(), id.raw_.begin());
  id.set_ = true;
  return id;
}

std::optional<RelayId> RelayId::Parse(const std::string_view text) {
  if (text.size() != kRelayIdTextLength || text.substr(0, kPrefixLength) != kRelayIdPrefix) {
    return std::nullopt;
  }
  const std::string encoded(text.substr(kPrefixLength));
  const bool url_alphabet = std::all_of(encoded.begin(), encoded.end(), [](const char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
  });
  if (!url_alphabet) {
    return std::nullopt;
  }
  const auto decoded = Base64UrlDecode(encoded);
  if (!decoded || decoded.value().size() != kRelayIdBytes || Base64UrlEncode(decoded.value()) != encoded) {
    return std::nullopt; // 43 characters carry 258 bits: the last one's two spare bits must be zero
  }
  return FromRaw(decoded.value());
}

RelayId RelayId::OfOperatorSet(const std::span<const uint8_t> operator_set) {
  const ByteVector hash = Blake2b256(RelayIdHashInput(operator_set));
  return FromRaw(hash).value_or(RelayId{});
}

std::string RelayId::ToString() const {
  if (!set_) {
    return {};
  }
  return std::string(kRelayIdPrefix) + Base64UrlEncode(RawBytes());
}

bool IsRelayIdForm(const std::string_view text) {
  return RelayId::Parse(text).has_value();
}

ByteVector RelayIdHashInput(const std::span<const uint8_t> operator_set) {
  ByteVector input(kRelayIdDomain, kRelayIdDomain + std::strlen(kRelayIdDomain));
  input.push_back(0x00);
  input.push_back(kRelayIdVersion);
  input.insert(input.end(), operator_set.begin(), operator_set.end());
  return input;
}

} // namespace pbr
