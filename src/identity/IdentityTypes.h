#pragma once

#include <string>

namespace pbr {

struct LocalIdentity {
  std::string public_key_b64;
  std::string encrypted_private_key_b64;
  std::string nickname;
  std::string relay_user_id;
  bool registered = false;
};

} // namespace pbr
