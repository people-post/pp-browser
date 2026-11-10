#pragma once

#include <string>
#include <utility>

namespace pbr {

/**
 * The awaiting ring page shows our own avatar, which can only be read once the vault is unlocked (the local identity
 * appears). Reading the avatar costs disk I/O, so the page asks this first: refresh only when the identity it last
 * drew with is not the identity there is now (empty to non-empty at unlock is the change that matters).
 */
class CallSelfAvatarWatch {
public:
  bool Changed(const std::string& identity_now) const { return identity_now != drawn_with_; }
  void Remember(std::string identity) { drawn_with_ = std::move(identity); }

private:
  std::string drawn_with_;
};

} // namespace pbr
