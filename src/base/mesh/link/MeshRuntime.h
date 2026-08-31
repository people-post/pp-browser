#pragma once

#include "base/adp/Endpoint.h"
#include "base/mesh/link/MeshPump.h"
#include "base/mesh/link/PeerLinkManager.h"
#include "base/mesh/session/Types.h"

#include <deque>
#include <functional>
#include <string>

namespace pbr::amp {

/**
 * Io-thread composer for Endpoint + PeerLinkManager + MeshPump.
 * L4 services must not touch ChannelSession/Mux off-thread except via PostToIo().
 */
class MeshRuntime {
public:
  using IoTask = std::function<void()>;

  MeshRuntime(adp::Endpoint& endpoint, MshIdentity local_identity, std::string local_peer_id,
              PeerLinkConfig config = {});

  adp::Endpoint& GetEndpoint() { return endpoint_; }
  PeerLinkManager& Links() { return links_; }
  const PeerLinkManager& Links() const { return links_; }

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** ADP pump + link tick (io-thread entry). */
  void Pump();
  void Tick();

  /** Queue work for the next Pump(); one queued task runs per Pump() before ADP I/O. */
  void PostToIo(IoTask task);

  /** Optional hook invoked at the start of each Pump() (e.g. connect deadlines). */
  void SetIoTick(IoTask tick) { io_tick_ = std::move(tick); }

private:
  adp::Endpoint& endpoint_;
  PeerLinkManager links_;
  MeshPump pump_;
  std::deque<IoTask> io_queue_;
  IoTask io_tick_;
  bool started_ = false;
  bool pumping_ = false;
};

} // namespace pbr::amp
