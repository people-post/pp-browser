#pragma once

#include "common/thread/IThreadStore.h"

#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** D014/D068/D085 — local epoch bump and compromised resolution (domain messaging). */
class EpochBumpCoordinator {
public:
  explicit EpochBumpCoordinator(IThreadStore& store);

  /** Epoch-only bump (D014) — Start new secure chat without PSK rotation. */
  Roe<uint32_t> StartNewSecureChat(const std::string& thread_id);

  /** D038 — user picks pause only; keeps sync_state=compromised with user_resolution. */
  Roe<void> PauseOnly(const std::string& thread_id);

private:
  IThreadStore& store_;
};

} // namespace pbr
