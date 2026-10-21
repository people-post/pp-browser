#pragma once

#include "domain/messaging/RelayReceivePipeline.h"
#include "domain/people/IdentityStore.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The receive pipeline's local keys, from the profile's identity (messaging does not include people). */
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
  };
}

} // namespace pbr
