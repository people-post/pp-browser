#pragma once

#include <string>

namespace pbr {

/**
 * Presentation projection of local identity (Me → Profile, tools, etc.).
 * Filled by messaging (`MessagingHub::LoadProfileIdentityView`); not the on-disk LocalIdentity.
 */
struct ProfileIdentityView {
  bool ready = false;
  std::string nickname;
  std::string peer_id;
  std::string relay_id;
  std::string public_key_b64;
  std::string registered = "no";
  std::string registration_status = "not registered";
  std::string registration_expires;
  std::string register_label = "Register on network";
  bool show_register = true;
  bool show_rotate = false;
  std::string brief_llm_key_masked;
};

} // namespace pbr
