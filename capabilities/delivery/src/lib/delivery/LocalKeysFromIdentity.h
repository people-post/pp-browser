#pragma once

#include "lib/people/DevicePeerId.h"
#include "lib/delivery/LocalKeys.h"
#include "lib/people/IdentityStore.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The local keys (decrypting, PSK rotation, group membership), from the profile's identity. */
inline LocalKeys LocalKeysFromIdentity(IdentityStore& identity) {
  return LocalKeys{
      [&identity]() -> Roe<std::string> {
        auto local = identity.Get();
        if (!local) {
          return local.error();
        }
        if (local->account_id.empty()) {
          return Error("No local account yet");
        }
        return local->account_id;
      },
      [&identity]() { return identity.GetOrCreateHybridKemPrivateKey(); },
      [&identity]() -> Roe<std::string> {
        auto local = identity.Get();
        if (!local) {
          return local.error();
        }
        return DevicePeerIdFromKey(local->public_key_b64);
      },
      [&identity]() { return identity.GetDeviceKemPrivateKey(); },
  };
}

} // namespace pbr
