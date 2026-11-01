#pragma once

#include "common/Diag.h"
#include "domain/call_media/ICallMediaTransport.h"
#include "lib/calls/CallMobility.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "mesh/reach/PeerReachRecord.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * Call-connection events for "Send diagnostics" (docs/contracts/DIAGNOSTICS.md § Diag events): enough, per
 * call and joinable across both devices by the call id, to say why a connect failed — without the developer
 * log. Lines are built here (so a test reads the exact text) and emitted by the caller; every value is a
 * count, a duration, a word from a fixed list, a `Ref` or a `Call`. Text detail stays in the developer log.
 */

/** Addresses by family and class — counts only, never an address. */
struct AddrCounts {
  int v4_private = 0;  // RFC 1918, link-local, loopback
  int v4_public = 0;
  int v6_global = 0;
  int v6_other = 0;  // link-local, unique-local
  bool operator==(const AddrCounts&) const = default;
};

/** `/ip4/…` and `/ip6/…` multiaddrs counted by class; other kinds are not counted. */
AddrCounts CountAddrs(const std::vector<std::string>& multiaddrs);

/** What the calls owner can see of this device and the mesh when a connect starts or ends. */
struct CallConnectEnv {
  AddrCounts local;
  bool attachment_known = false;
  MobilityAttachment attachment;
  /** Rendezvous relays the app's policy offers, how many have a link up, how many the reach record holds down. */
  int relays_surface = 0;
  int relays_connected = 0;
  int relays_held_down = 0;
  mesh::PeerReachState peer_state = mesh::PeerReachState::Unknown;
  mesh::PeerReachFailure peer_fail = mesh::PeerReachFailure::None;
  int peer_fails_in_row = 0;
  /** Family of the dial book's preferred endpoint for the peer: 4, 6, or 0 (none). */
  int peer_pref_family = 0;
};

/** Read on the calls owner; the argument is the mesh PeerId the connect dials. */
using CallConnectEnvProbe = std::function<CallConnectEnv(const std::string& peer_key)>;

/** The events, as the budget names them. */
enum class CallDiagEvent : int { Net = 0, Signal, Reach, Relay, Punch, Result, kCount };

/**
 * Caps each event per call (a retry loop must not flood the file) and remembers only the last few calls.
 * Thread-safe (the punch result arrives off the calls owner); a leaf lock, never held while emitting.
 */
class CallDiagBudget {
public:
  static constexpr int kPerEventPerCall = 8;
  static constexpr size_t kCalls = 8;

  /** True while `event` has room for this call; counts the line. */
  bool Allow(std::string_view call_id, CallDiagEvent event);

  /** The process-wide budget the product emits through. */
  static CallDiagBudget& Shared();

private:
  using Counts = std::array<uint8_t, static_cast<size_t>(CallDiagEvent::kCount)>;
  std::mutex mu_;
  std::map<std::string, Counts> counts_;
  std::vector<std::string> order_;
};

// Words: fixed lists, so a runtime value reaches the file only as one of them.
ShareWord LinkKindWord(mesh::PeerLinkKind kind);
ShareWord ReachErrWord(mesh::PeerReachCoordinator::Err code);
/** The circuit step's last refusal, classified from its message chain (the text itself never leaves). */
ShareWord ReachWhyWord(const std::string& message);
ShareWord PeerStateWord(mesh::PeerReachState state);
ShareWord PeerFailWord(mesh::PeerReachFailure failure);
ShareWord MediaErrWord(CallMediaErr code);
/** 4 / 6 / 0 from a multiaddr's leading `/ip4/` or `/ip6/`. */
int AddrFamily(const std::string& multiaddr);

// Line builders; the caller checks the budget, then Emit()s.
DiagLine CallNetLine(std::string_view call_id, bool offerer, const CallConnectEnv& env);
DiagLine CallSignalLine(ShareWord phase, std::string_view call_id, const std::vector<std::string>& addrs,
                        int64_t age_ms, bool implicit);
DiagLine CallReachOkLine(std::string_view call_id, bool offerer, int attempt, mesh::PeerLinkKind kind, bool reused,
                         int family, int64_t ms);
DiagLine CallReachFailLine(std::string_view call_id, bool offerer, int attempt,
                           const mesh::PeerReachCoordinator::Failure& failure, int64_t ms);
DiagLine CallRelayLine(ShareWord dir, std::string_view call_id, std::string_view relay_peer);
DiagLine CallPunchLine(ShareWord phase, std::string_view call_id, int window_ms, size_t addrs, int64_t ms);

/** One connect sequence's end. `kind` / `family` are from the last link that settled (unknown/0 if none). */
struct CallResultFacts {
  bool offerer = false;
  bool ok = false;
  int attempts = 0;
  int64_t ms = 0;
  mesh::PeerLinkKind kind = mesh::PeerLinkKind::Unknown;
  int family = 0;
  CallMediaErr media_err = CallMediaErr::Ok;
  bool reach_failed = false;
  mesh::PeerReachCoordinator::Err reach_err = mesh::PeerReachCoordinator::Err::Ok;
  ShareWord why = "none";
};
DiagLine CallResultLine(std::string_view call_id, const CallResultFacts& facts, const CallConnectEnv& env);

} // namespace pbr
