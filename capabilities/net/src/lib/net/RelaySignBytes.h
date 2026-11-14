#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {

void RelaySignAppendDomain(std::ostringstream& oss, const char* domain_with_nul);
/**
 * A signed relay command's header (multi-relay MR007): `domain ‖ 0x00 ‖ u8 version ‖ LenUtf8(audience)`, the
 * audience the PeerId of the one relay the command is for (base58btc, as nodes print it). False, and nothing
 * written, when `audience` is empty: no command is signed for no relay.
 */
bool RelaySignAppendCommandHeader(std::ostringstream& oss, const char* domain, uint8_t version,
                                  const std::string& audience);
void RelaySignAppendU8(std::ostringstream& oss, uint8_t value);
void RelaySignAppendI64(std::ostringstream& oss, int64_t value);
void RelaySignAppendU64(std::ostringstream& oss, uint64_t value);
void RelaySignAppendWireLenUtf8(std::ostringstream& oss, const std::string& value);
std::vector<uint8_t> RelaySignOssToBytes(std::ostringstream& oss);

uint8_t SignatureAlgToWire(const std::string& signature_alg);

} // namespace pbr
