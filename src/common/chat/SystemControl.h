#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Who sent an inbound system-control message, and when the relay saw it. */
struct SystemControlContext {
  std::string sender_identity;
  std::optional<int64_t> relay_created_at_ms;
  std::optional<int64_t> relay_server_time_ms;
};

/** How a transcript names people in a family's lines. */
struct SystemControlLabels {
  /** A peer's display label; empty when unknown. */
  std::function<std::string(const std::string& identity)> peer;
};

/**
 * A family of system-control messages a feature owns (call control, billing, …): system messages
 * whose payload names a `control_type`. Messaging routes them here by what the family claims,
 * without knowing what they mean. They travel one of two ways:
 *
 * - **The chat stream** (default): end to end, ordered, stored as transcript rows — history the user
 *   reads (a call summary, a payment receipt).
 * - **The control lane** (`on_lane`): signaling with no place in a transcript (call invite, accept,
 *   media keys, SDP / ICE …). Same E2E session, outside the ordered stream; never stored; applied
 *   at most once, and only while fresh.
 */
struct SystemControlFamily {
  /** For logs and receive-failure notices ("call"). */
  std::string name;
  /** Whether a message is this family's. */
  std::function<bool(const ThreadMessage&)> owns;
  /** An inbound message's side effects. An error refuses the message. */
  std::function<Roe<void>(ThreadMessage& message, const SystemControlContext& context)> apply;
  /**
   * Whether one of the family's messages travels on the control lane. A lane message that arrives in
   * the chat stream is dropped (never applied, never stored). None: all in the stream.
   */
  std::function<bool(const ThreadMessage&)> on_lane;
  /**
   * Lane: how long a message may have waited in the relay (relay clock) and still apply. The lane
   * also bounds the sender's signed time and drops a repeated id.
   */
  int64_t lane_max_age_ms = 5 * 60 * 1000;
  /** A stream message shown, but with no unread count, preview or notice: it is not news. */
  std::function<bool(const ThreadMessage&)> quiet;
  /** A stream message's transcript line, names included; none: the message's own text. */
  std::function<std::optional<std::string>(const ThreadMessage&, const SystemControlLabels&)> line;
  /**
   * While true, the family has a live exchange running (a call): messaging holds key rotation (it
   * would strand lane messages in flight) and defers background history sync (the lane must not
   * queue behind it).
   */
  std::function<bool()> live;
};

/**
 * The families messaging routes to. Bound and unbound from any thread (a feature rebuilds itself);
 * readers take an immutable snapshot.
 */
class SystemControlRegistry {
public:
  using Families = std::vector<SystemControlFamily>;

  /** Add `family`, or replace the one of the same name. */
  void Bind(SystemControlFamily family);
  void Unbind(const std::string& name);
  std::shared_ptr<const Families> Snapshot() const;

  /** The family that owns `message`; none for a plain message or an unbound family. */
  std::optional<SystemControlFamily> Find(const ThreadMessage& message) const;
  /** True when a bound family owns `message` and sends it on the control lane. */
  bool OnLane(const ThreadMessage& message) const;
  bool Quiet(const ThreadMessage& message) const;
  /** The name of a family with a live exchange running; none when all are idle. */
  std::optional<std::string> LiveFamily() const;

private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Families> families_ = std::make_shared<const Families>();
};

} // namespace pbr
