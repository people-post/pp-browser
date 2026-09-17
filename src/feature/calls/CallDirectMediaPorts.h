#pragma once

#include "common/Error.h"
#include "feature/calls/CallMediaSeat.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

class CallMediaBridge;

/**
 * Stack-filled Direct media façade for CallSessionManager (V042).
 * CSM must not hold CallMediaBridge* — ops copy these functions.
 */
struct CallDirectMediaPorts {
  std::function<void(const std::string& call_id, const std::string& peer_identity, bool offerer)>
      schedule_start;
  std::function<std::string()> media_path_kind;
  std::function<void(const std::string& peer_id, const std::string& relay_identity)>
      note_peer_id_relay_mapping;
  std::function<void(const std::string& call_id)> stop_mesh_media;
  std::function<bool()> is_connect_failed;
  std::function<bool()> connect_missing_mic;
  std::function<void()> poll_connect_health;
  std::function<Roe<void>(const std::string& call_id)> retry_mesh_media;
  std::function<bool(const std::string& call_id)> media_attempted;
  std::function<void(const std::string& call_id)> note_media_attempted;
  /** SoftMigrate: ReleaseDirect under seat token when seat present. */
  std::function<void()> release_direct_transport;
  std::function<void(const std::string& call_id)> on_media_key_ready;

  bool IsBound() const { return static_cast<bool>(schedule_start); }
};

/** Build ports over a live bridge (+ optional seat for CallDirectPath). Null bridge → empty ports. */
CallDirectMediaPorts MakeCallDirectMediaPorts(CallMediaBridge* bridge, CallMediaSeat* seat);

} // namespace pbr
