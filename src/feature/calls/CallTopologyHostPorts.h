#pragma once

#include "domain/messaging/CallControlCodec.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * CSM → Topology side effects (V046).
 * Topology projects a migrate subset into CallHopMigrateHostPorts for owned Workflow.
 */
struct CallTopologyHostPorts {
  std::function<Roe<std::string>()> local_relay_identity;
  std::function<Roe<void>(const std::string& call_id)> leave_call;
  std::function<Roe<void>(const std::string& call_id, CallControlType type, const std::string& detail_json,
                          const std::string& display, const std::string& skip_identity)>
      fan_out_joined;
  std::function<Roe<void>(const std::string& peer_identity, CallControlType type,
                          const std::string& detail_json, const std::string& display)>
      send_direct;
  std::function<void()> notify_ring_changed;
  std::function<void(std::string message)> set_last_media_error;
  /** Ephemeral in-call progress (hop pick / switch) — not an error banner. */
  std::function<void(std::string message)> set_media_activity;
  std::function<void()> clear_media_activity;
  std::function<void(const std::string& call_id)> note_media_attempted;
  std::function<void(const std::string& call_id)> bind_media_call_id;
  std::function<void()> clear_media_peer_identity;
  /**
   * SoftMigrate to media_relay: drop 1:1 call-media stream without CallMediaEngine::Stop
   * (capture must keep running into SFU send).
   */
  std::function<void()> release_direct_media;
  /** Guest WaitForAttach — force relay inbox poll for CallSfuAttach. */
  std::function<void()> request_inbox_sync;

  bool IsBound() const { return static_cast<bool>(local_relay_identity); }

  /** Optional void ports — never invoke empty std::function (V046 null-guards). */
  void ClearMediaActivity() const {
    if (clear_media_activity) {
      clear_media_activity();
    }
  }
  void NotifyRingChanged() const {
    if (notify_ring_changed) {
      notify_ring_changed();
    }
  }
  void ClearMediaPeerIdentity() const {
    if (clear_media_peer_identity) {
      clear_media_peer_identity();
    }
  }
  void ReleaseDirectMedia() const {
    if (release_direct_media) {
      release_direct_media();
    }
  }
  void RequestInboxSync() const {
    if (request_inbox_sync) {
      request_inbox_sync();
    }
  }
  void SetMediaActivity(std::string message) const {
    if (set_media_activity) {
      set_media_activity(std::move(message));
    }
  }
  void SetLastMediaError(std::string message) const {
    if (set_last_media_error) {
      set_last_media_error(std::move(message));
    }
  }
};

} // namespace pbr
