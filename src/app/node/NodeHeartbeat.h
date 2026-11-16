#pragma once

#include "common/Error.h"
#include "common/net/HttpTransport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/** Liveness state reported to the mesh directory. Only ever advances: active -> draining -> retiring. */
enum class HeartbeatState : int { Active = 0, Draining = 1, Retiring = 2 };

const char* HeartbeatStateName(HeartbeatState state);
std::optional<HeartbeatState> ParseHeartbeatState(std::string_view name);

inline constexpr int64_t kHeartbeatPeriodMs = 5 * 60 * 1000;
inline constexpr long kHeartbeatTimeoutS = 8;
inline constexpr long kHeartbeatFinalTimeoutS = 3;

/** Both empty until the node is registered. */
struct HeartbeatIdentity {
  std::string account_id;
  std::string peer_id;
};

/** A signed heartbeat: the relay it is for (its PeerId, multi-relay MR007) and the account key's signature over it. */
struct HeartbeatSignature {
  std::string audience;
  std::string signature;
};

struct NodeHeartbeatDeps {
  /** `registration.base_url`; the heartbeat goes to `<base_url>/v1/mesh/heartbeat`. */
  std::string base_url;
  std::function<HeartbeatIdentity()> identity;
  /**
   * Signs the heartbeat sign bytes for the relay's pinned PeerId (the audience) with the account key. An error when
   * nothing may be signed for that relay (its identity answer does not match its address, or none is named): no
   * heartbeat goes.
   */
  std::function<Roe<HeartbeatSignature>(const std::string& account_id, const std::string& peer_id,
                                        const std::string& state, int64_t timestamp)>
      sign;
  /** The relay refused a heartbeat with this `code` (`wrong_audience`: its identity is asked again). May be empty. */
  std::function<void(const std::string& code)> refused;
  HttpPostFn post;
  /** The same entry point as startup / periodic registration renewal. */
  std::function<Roe<bool>()> reregister;
  std::function<int64_t()> unix_ms;
  std::function<int64_t()> steady_ms;
};

/**
 * Periodic signed liveness ping from pp-node to the mesh directory. Tick() and SendFinal() belong to the
 * main loop thread; SetState() / State() may be called from any thread.
 */
class NodeHeartbeat {
public:
  explicit NodeHeartbeat(NodeHeartbeatDeps deps);

  /** False when the move is refused (retiring is final). Setting the current state is a no-op success. */
  bool SetState(HeartbeatState next);
  HeartbeatState State() const { return static_cast<HeartbeatState>(state_.load()); }

  /** Sends when the period elapsed or the state changed; re-registers on 404. */
  void Tick();

  /** Shutdown path: marks retiring and sends once with a short timeout; failure is only a warning. */
  void SendFinal();

private:
  enum class Outcome { Ok, Auth, Stale, NotRegistered, UnknownPeer, Rejected, HttpError, Network };

  Outcome Send(HeartbeatState state, long timeout_s);
  void RecordOutcome(Outcome outcome);
  void HandleNotRegistered(int64_t now);
  void PublishState(HeartbeatState state);

  NodeHeartbeatDeps deps_;
  std::atomic<int> state_{static_cast<int>(HeartbeatState::Active)};
  HeartbeatState attempted_state_ = HeartbeatState::Active;
  int64_t next_send_ms_ = INT64_MIN;
  int64_t last_timestamp_ = 0;
  int64_t reregister_due_ms_ = INT64_MIN;
  int64_t reregister_delay_ms_ = 0;
  /** The last signing failure logged (one line per new reason, not one per period). */
  std::string sign_failure_;
};

} // namespace pbr
