#include "base/net/RelaySignBytes.h"

#include "common/Serialize.hpp"

namespace pbr {

void RelaySignAppendDomain(std::ostringstream& oss, const char* domain_with_nul) {
  for (const char* p = domain_with_nul; *p != '\0'; ++p) {
    oss.put(*p);
  }
  oss.put('\0');
}

void RelaySignAppendU8(std::ostringstream& oss, const uint8_t value) {
  oss.write(reinterpret_cast<const char*>(&value), 1);
}

void RelaySignAppendI64(std::ostringstream& oss, const int64_t value) {
  OutputArchive ar(oss);
  ar & value;
}

void RelaySignAppendU64(std::ostringstream& oss, const uint64_t value) {
  OutputArchive ar(oss);
  ar & value;
}

void RelaySignAppendWireLenUtf8(std::ostringstream& oss, const std::string& value) {
  WireLenUtf8 wire{value};
  OutputArchive ar(oss);
  ar & wire;
}

std::vector<uint8_t> RelaySignOssToBytes(std::ostringstream& oss) {
  const std::string packed = oss.str();
  return std::vector<uint8_t>(packed.begin(), packed.end());
}

uint8_t SignatureAlgToWire(const std::string& signature_alg) {
  if (signature_alg == "ml-dsa-65") {
    return 1;
  }
  return 0xFF;
}

} // namespace pbr
