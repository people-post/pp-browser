#pragma once

#include "feature/calls/CallEngine.h"
#include "feature/calls/CallHopRelayDeps.h"
#include "feature/calls/SfuKeyRing.h"
#include "lib/media/MediaEngine.h"
#include "mesh/media_plane/MediaRelayAttach.h"

#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * One call's media on its hop (V057 6): the data plane of its group path (CallHopPath, which owns
 * it and decides everything). Seals and opens the call's frames with the attach's key ring, hands
 * them to the call's engine handle and the relay client, and keeps the publisher stream ids. The
 * relay's I/O threads hold it (shared) through the frame sink and send function it builds, never
 * the path; everything here is safe off the calls owner.
 */
class CallHopMediaChannel : public Module, public std::enable_shared_from_this<CallHopMediaChannel> {
public:
  CallHopMediaChannel(std::string call_id, std::shared_ptr<CallEngine> engine, const CallHopMediaRelayDeps& relay);

  // --- Keys (V003) ----------------------------------------------------------------------------
  /** A committed attach's ring takes the call's later rotations. */
  void MakeLive(std::shared_ptr<SfuKeyRing> keys);
  bool HasLiveKeys() const;
  /** A rotation: the live ring opens with `key` at `epoch` from now (no-op without a live ring). */
  void InstallKey(uint32_t epoch, const ByteVector& key);

  // --- Frames -----------------------------------------------------------------------------------
  /** Inbound hop frames → opened → the call's engine; dropped until `ready`. */
  std::function<void(mesh::MediaDataFrame)> MakeFrameSink(std::shared_ptr<SfuKeyRing> keys,
                                                          std::shared_ptr<std::atomic<bool>> ready);
  /** The engine's packets → sealed with the ring's newest key → the relay, as our publisher stream. */
  MediaEngine::PacketSendFn MakeSendFn(std::shared_ptr<SfuKeyRing> keys);

  // --- Publisher streams ------------------------------------------------------------------------
  uint32_t LocalStreamId() const { return local_stream_id_.load(); }
  void SetLocalStreamId(uint32_t id) { local_stream_id_.store(id); }
  void AddRemoteStream(uint32_t id);
  std::vector<uint32_t> RemoteStreams() const;
  size_t RemoteStreamCount() const;
  /** True the first time `stream_id` asks for an IDR (later asks are not sent again). */
  bool FirstVideoRefresh(uint32_t stream_id);
  /** The hop is left: no local publisher, no remote streams. */
  void ResetStreams();

private:
  const std::string call_id_;
  const std::shared_ptr<CallEngine> engine_;
  /** The device's relay deps (CallHopService), which outlive every call's path. */
  const CallHopMediaRelayDeps& relay_;
  mutable std::mutex mu_;
  std::shared_ptr<SfuKeyRing> live_keys_;
  std::atomic<uint32_t> local_stream_id_{0};
  std::unordered_set<uint32_t> remote_stream_ids_;
  std::unordered_set<uint32_t> video_refresh_sent_;
};

} // namespace pbr
