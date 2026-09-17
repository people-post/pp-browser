#include "feature/calls/CallDirectMediaPorts.h"

#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaPaths.h"

namespace pbr {

CallDirectMediaPorts MakeCallDirectMediaPorts(CallMediaBridge* bridge, CallMediaSeat* seat) {
  CallDirectMediaPorts ports;
  if (!bridge) {
    return ports;
  }
  ports.schedule_start = [bridge, seat](const std::string& call_id, const std::string& peer,
                                        bool offerer) {
    CallDirectPath(bridge, seat).ScheduleStart(call_id, peer, offerer);
  };
  ports.media_path_kind = [bridge]() { return bridge->MediaPathKind(); };
  ports.note_peer_id_relay_mapping = [bridge](const std::string& peer_id,
                                              const std::string& relay_identity) {
    bridge->NotePeerIdRelayMapping(peer_id, relay_identity);
  };
  ports.stop_mesh_media = [bridge](const std::string& call_id) { bridge->StopMeshMedia(call_id); };
  ports.is_connect_failed = [bridge]() { return bridge->IsMeshConnectFailed(); };
  ports.connect_missing_mic = [bridge]() {
    return bridge->IsMeshConnectFailed() && bridge->MeshConnectMissingMic();
  };
  ports.poll_connect_health = [bridge]() { bridge->PollMeshConnectHealth(); };
  ports.retry_mesh_media = [bridge](const std::string& call_id) {
    return bridge->RetryMeshMedia(call_id);
  };
  ports.media_attempted = [bridge](const std::string& call_id) {
    return bridge->MediaAttempted(call_id);
  };
  ports.note_media_attempted = [bridge](const std::string& call_id) {
    bridge->NoteMediaAttempted(call_id);
  };
  ports.release_direct_transport = [bridge, seat]() {
    if (seat) {
      (void)CallDirectPath(bridge, seat).ReleaseTransport(seat->CurrentToken());
      return;
    }
    bridge->ReleaseDirectTransport();
  };
  ports.on_media_key_ready = [bridge](const std::string& call_id) {
    bridge->OnMediaKeyReady(call_id);
  };
  return ports;
}

} // namespace pbr
