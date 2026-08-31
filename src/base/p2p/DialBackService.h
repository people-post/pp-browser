#pragma once

#include "base/p2p/DialBackTypes.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <memory>
#include <string>
#include <vector>

namespace pbr {

/**
 * Legacy TCP dial-back (unlinked from product). Prefer AmpDialBackService (D8).
 */
class DialBackService {
public:
  DialBackService(Libp2pHost& host, PeerSessionManager& sessions);
  ~DialBackService();

  DialBackService(const DialBackService&) = delete;
  DialBackService& operator=(const DialBackService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  Roe<DialBackProbeResult> Probe(const std::string& seed_peer_key,
                                 const std::vector<std::string>& target_multiaddrs,
                                 int timeout_ms = 8000);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
