#include "feature/messaging/EpochBumpCoordinator.h"
#include "common/PbrCompat.h"

namespace pbr {

EpochBumpCoordinator::EpochBumpCoordinator(IThreadStore& store) : store_(store) {}

Roe<uint32_t> EpochBumpCoordinator::StartNewSecureChat(const std::string& thread_id) {
  return store_.BumpLocalChatTargetEpoch(thread_id);
}

Roe<void> EpochBumpCoordinator::PauseOnly(const std::string& thread_id) {
  const auto epoch = store_.GetChatTargetSessionEpoch(thread_id);
  if (!epoch) {
    return epoch.error();
  }
  auto sync_state = store_.GetPeerSyncState(thread_id, *epoch);
  if (!sync_state) {
    return sync_state.error();
  }
  PeerSyncState updated = *sync_state;
  updated.phase = PeerSyncPhase::Compromised;
  updated.user_resolution = "pause_only";
  return store_.SetPeerSyncState(thread_id, *epoch, updated);
}

} // namespace pbr
