#pragma once

#include "base/people/ProfileIdentityView.h"
#include "common/Error.h"

#include <functional>
#include <string>

namespace pbr {

/** Input for register / renew — not the full Me form bag. */
struct RegisterIdentityArgs {
  /** Empty = leave the stored identity nickname unchanged. */
  std::string nickname;
};

/**
 * Imperative settings ports. Declared here (consumer); Application fills
 * implementations that call messaging / profile lifecycle. Not a singleton.
 * Args are operation-shaped; callers refresh SettingsUiState after success.
 */
struct SettingsCommands {
  std::function<ProfileIdentityView()> load_profile_identity;
  std::function<Roe<void>(const std::string& nickname)> save_profile_nickname;
  std::function<Roe<void>(const RegisterIdentityArgs& args)> register_identity;
  std::function<Roe<void>()> rotate_brief_llm_key;
  std::function<Roe<void>(int older_than_days)> clear_undelivered_older_than;
  std::function<void(bool try_upnp)> run_reachability_probe;
  std::function<void()> try_upnp_port_mapping;
  /** Wipe profile data and reinit hub/secrets — app owns that lifecycle. */
  std::function<Roe<void>()> reset_active_profile;
};

} // namespace pbr
