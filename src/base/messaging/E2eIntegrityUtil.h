#pragma once

#include "base/messaging/IThreadStore.h"
#include "base/messaging/SyncStateTypes.h"

#include <string>

namespace pbr {

inline bool IsE2eThreadCompromised(IThreadStore& store, const std::string& thread_id) {
  const auto epoch = store.GetChatTargetSessionEpoch(thread_id);
  if (!epoch) {
    return false;
  }
  const auto sync_state = store.GetPeerSyncState(thread_id, *epoch);
  if (!sync_state) {
    return false;
  }
  return (*sync_state).phase == PeerSyncPhase::Compromised;
}

} // namespace pbr
