#pragma once

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

struct Ed25519KeyPair {
  std::vector<uint8_t> public_key;
  std::vector<uint8_t> private_key;
};

class Ed25519Signer {
public:
  static Roe<Ed25519KeyPair> GenerateKeyPair();
  static Roe<std::string> Sign(const std::string& message, const std::vector<uint8_t>& private_key);
  static Roe<bool> Verify(const std::string& message, const std::string& signature_b64,
                          const std::vector<uint8_t>& public_key);

  static std::string ToBase64(const std::vector<uint8_t>& data);
  static Roe<std::vector<uint8_t>> FromBase64(const std::string& encoded);
};

} // namespace pbr
