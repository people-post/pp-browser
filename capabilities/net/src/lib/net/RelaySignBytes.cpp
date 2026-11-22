#include "lib/net/RelaySignBytes.h"

#include <algorithm>
#include <cstddef>

namespace pbr {

std::vector<uint8_t> BuildRelayCallBytes(const std::string_view domain, const std::string& audience,
                                         const pp::ExtArea& ext, const RelayCallFields& fields) {
  if (audience.empty()) {
    return {};
  }
  SignedBytesWriter writer(domain, kRelaySignVersion);
  writer.Utf8(audience);
  if (fields) {
    fields(writer);
  }
  return writer.Finish(ext);
}

int RelaySignVersionOf(const std::vector<uint8_t>& bytes) {
  const auto version = SignedBytesVersion(bytes);
  return version ? static_cast<int>(*version) : -1;
}

bool RelayCallBytesEndWith(const std::vector<uint8_t>& bytes, const pp::ExtArea& ext) {
  const std::vector<uint8_t> area = ext.Encode();
  return bytes.size() >= area.size() &&
         std::equal(area.begin(), area.end(), bytes.end() - static_cast<std::ptrdiff_t>(area.size()));
}

} // namespace pbr
