#pragma once

#include "common/Error.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <memory>
#include <string>
#include <unordered_set>

namespace pbr {

inline constexpr const char* kCircuitRelayProtocolId = "/pp-browser/circuit-relay/1.0.0";

struct CircuitRelayBridgeResult {
  bool ok = false;
  std::string error;
};

/** Provider admission (nf): volunteer desktop may prefer contacts. */
struct CircuitRelayAdmissionPolicy {
  /** When true and contact_peer_ids non-empty, refuse non-contact dialers. */
  bool prefer_contacts_only = false;
  std::unordered_set<std::string> contact_peer_ids;
};

/**
 * Custom circuit relay (n3): relay host bridges a stream to a target multiaddr.
 * Not libp2p circuit-relay v2 — integration-layer protocol like DialBackService.
 * nf: optional contact-preferring admission via SetAdmissionPolicy.
 */
class CircuitRelayService {
public:
  CircuitRelayService(Libp2pHost& host, PeerSessionManager& sessions);
  ~CircuitRelayService();

  CircuitRelayService(const CircuitRelayService&) = delete;
  CircuitRelayService& operator=(const CircuitRelayService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Hot-update provider admission (MessagingHub feeds contact PeerIds). */
  void SetAdmissionPolicy(CircuitRelayAdmissionPolicy policy);

  /**
   * Client: ask relay peer to bridge this stream to `target_multiaddr`.
   * Returns after relay accepts or rejects (stream stays open on success for app use).
   */
  Roe<CircuitRelayBridgeResult> RequestBridge(const std::string& relay_peer_key,
                                              const std::string& target_multiaddr,
                                              int timeout_ms = 8000);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
