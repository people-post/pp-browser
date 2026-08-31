#pragma once

#include "base/adp/Clock.h"
#include "base/adp/DatagramIo.h"
#include "base/adp/Endpoint.h"
#include "base/mesh/link/MeshRuntime.h"
#include "base/mesh/session/Types.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>

namespace pbr::amp {

/**
 * Owns ADP Endpoint + MeshRuntime for one local peer (D9 composition building block).
 * Product MeshHost may hold an AmpStack in parallel before traffic cutover ([A020]).
 */
class AmpStack {
public:
  struct Config {
    MshIdentity identity;
    std::string local_peer_id;
    PeerLinkConfig link_config;
  };

  static Roe<std::unique_ptr<AmpStack>> Create(std::shared_ptr<adp::DatagramIo> io,
                                               std::shared_ptr<adp::Clock> clock, Config config);

  adp::Endpoint& GetEndpoint() { return *endpoint_; }
  MeshRuntime& Runtime() { return *runtime_; }
  PeerLinkManager& Links() { return runtime_->Links(); }
  const std::string& LocalPeerId() const { return local_peer_id_; }

  void Start();
  void Stop();
  bool IsStarted() const { return runtime_ && runtime_->IsStarted(); }

  void Pump();
  void Tick();
  void PostToIo(MeshRuntime::IoTask task);

private:
  AmpStack() = default;

  std::shared_ptr<adp::DatagramIo> io_;
  std::shared_ptr<adp::Clock> clock_;
  std::unique_ptr<adp::Endpoint> endpoint_;
  std::unique_ptr<MeshRuntime> runtime_;
  std::string local_peer_id_;
};

} // namespace pbr::amp
