#pragma once

#include "foundation/crypto/IDekConsumer.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionStore.h"

#include <memory>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** A profile's call stores, as calls use them (CR024): ports only. The app's storage opens them; tests in memory. */
struct CallStores {
  std::unique_ptr<ICallSessionStore> sessions;
  std::unique_ptr<ICallMediaKeyStore> media_keys;
  /** The stores above that are encrypted at rest: the app hands them the profile's data key. */
  std::vector<IDekConsumer*> at_rest;
};

} // namespace pbr
