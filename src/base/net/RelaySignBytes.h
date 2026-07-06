#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {

void RelaySignAppendDomain(std::ostringstream& oss, const char* domain_with_nul);
void RelaySignAppendU8(std::ostringstream& oss, uint8_t value);
void RelaySignAppendI64(std::ostringstream& oss, int64_t value);
void RelaySignAppendU64(std::ostringstream& oss, uint64_t value);
void RelaySignAppendWireLenUtf8(std::ostringstream& oss, const std::string& value);
std::vector<uint8_t> RelaySignOssToBytes(std::ostringstream& oss);

uint8_t SignatureAlgToWire(const std::string& signature_alg);

} // namespace pbr
