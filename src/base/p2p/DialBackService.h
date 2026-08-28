#pragma once

#include "common/Error.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"

#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

inline constexpr const char* kDialBackProtocolId = "/pp-browser/dial-back/1.0.0";

struct DialBackProbeResult {
  bool ok = false;
  std::string dialed;
  std::string error;
};

/**
 * Seed dial-back probe (np / N012): client asks seed to dial advertised listen addrs.
 * Framing matches ChatHistoryStreamCodec (u64-BE length + UTF-8 JSON).
 *
 * Handlers hop off the libp2p io thread before blocking reads/dials.
 * Note: if the seed is already connected to the target PeerId, dial may reuse that
 * connection (nr may tighten address-forced dials later).
 */
class DialBackService {
public:
  DialBackService(Libp2pHost& host, PeerSessionManager& sessions);
  ~DialBackService();

  DialBackService(const DialBackService&) = delete;
  DialBackService& operator=(const DialBackService&) = delete;

  /** Register inbound handler (seed / Node). */
  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /**
   * Client: open dial-back stream to `seed_peer_key` (must be RegisterEndpoint'd),
   * ask it to dial `target_multiaddrs`.
   */
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
