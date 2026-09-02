#include "foundation/identity/MlDsaPublicKeyWire.h"

namespace pbr {

namespace {

void AppendVarint(std::vector<uint8_t>& out, uint64_t value) {
  while (value >= 0x80) {
    out.push_back(static_cast<uint8_t>((value & 0x7f) | 0x80));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
}

void AppendTag(std::vector<uint8_t>& out, uint32_t field_number, uint8_t wire_type) {
  AppendVarint(out, (static_cast<uint64_t>(field_number) << 3) | wire_type);
}

void AppendBytesField(std::vector<uint8_t>& out, uint32_t field_number, const std::vector<uint8_t>& data) {
  if (data.empty()) {
    return;
  }
  AppendTag(out, field_number, 2);
  AppendVarint(out, data.size());
  out.insert(out.end(), data.begin(), data.end());
}

} // namespace

std::vector<uint8_t> EncodeMlDsa65PublicKeyWire(const std::vector<uint8_t>& public_key) {
  std::vector<uint8_t> out;
  out.reserve(public_key.size() + 16);
  // KeyTypeWire::kMlDsa65 = 4
  AppendTag(out, 1, 0);
  AppendVarint(out, 4);
  AppendBytesField(out, 2, public_key);
  return out;
}

} // namespace pbr
