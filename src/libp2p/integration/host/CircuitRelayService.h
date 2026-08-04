#pragma once

#include "base/people/RelayScope.h"
#include "common/Error.h"
#include "libp2p/integration/host/CircuitBridgeTarget.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <libp2p/connection/stream.hpp>
#include <memory>
#include <string>
#include <unordered_set>

namespace pbr {

inline constexpr const char* kCircuitRelayProtocolId = "/pp-browser/circuit-relay/1.0.0";

struct CircuitRelayBridgeResult {
  bool ok = false;
  std::string error;
  /** Populated when relay resolved target_peer_id (L3). */
  std::string resolved_multiaddr;
  /** Client-side stream after successful bridge (may be null). */
  std::shared_ptr<libp2p::connection::Stream> stream;
};

/** Provider admission (nf / N023): scope mask + contact PeerIds. */
struct CircuitRelayAdmissionPolicy {
  /** When true and contact_peer_ids non-empty, refuse non-contact dialers (legacy; see serve_scope_mask). */
  bool prefer_contacts_only = false;
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
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

  /** Unblock in-flight RequestBridge waiters (Leave / hub shutdown). */
  void AbortInflightRequests();

  /**
   * Client: ask relay peer to bridge this stream to a target (multiaddr and/or PeerId).
   * Returns after relay accepts or rejects (stream stays open on success for app use).
   */
  Roe<CircuitRelayBridgeResult> RequestBridge(const std::string& relay_peer_key,
                                              const CircuitBridgeTarget& target, int timeout_ms = 8000);

  /** Legacy: bridge to explicit multiaddr. */
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
