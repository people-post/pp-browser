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
 * A family of system-control messages a feature owns (call control, …): system messages whose
 * payload names a `control_type`. Messaging carries them like any message — end to end, ordered,
 * stored — and routes them here by what the family claims, without knowing what they mean.
 */
struct SystemControlFamily {
  /** For logs and receive-failure notices ("call"). */
  std::string name;
  /** Whether a message is this family's. */
  std::function<bool(const ThreadMessage&)> owns;
  /**
   * An inbound message's side effects. Must be idempotent: it may run again for a message sent
   * again. An error refuses the message.
   */
  std::function<Roe<void>(ThreadMessage& message, const SystemControlContext& context)> apply;
  /**
   * Also applied when the ordered chat stream drops the message — a duplicate, a resent sequence,
   * a compromised or rejected stream: its signature is checked, its order is not.
   */
  bool apply_out_of_order = false;
  /** Never shown in a transcript (plumbing). */
  std::function<bool(const ThreadMessage&)> hidden;
  /** Shown, but no unread count, preview or notice: it is not news. */
  std::function<bool(const ThreadMessage&)> quiet;
  /** A shown message's transcript line, names included; none: the message's own text. */
  std::function<std::optional<std::string>(const ThreadMessage&, const SystemControlLabels&)> line;
  /**
   * While true, the family has a live exchange running (a call): messaging holds key rotation (it
   * would strand messages in flight) and defers background history sync (control must not queue
   * behind it).
   */
  std::function<bool()> live;
  /** A thread the family prefers its public control messages on (the one it is acting in). */
  std::function<std::optional<std::string>()> preferred_public_thread;
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
  /** True when a bound family is owned and applies out of order. */
  bool AppliesOutOfOrder(const ThreadMessage& message) const;
  bool Hidden(const ThreadMessage& message) const;
  bool Quiet(const ThreadMessage& message) const;
  /** The name of a family with a live exchange running; none when all are idle. */
  std::optional<std::string> LiveFamily() const;
  std::optional<std::string> PreferredPublicThread() const;

private:
  mutable std::mutex mutex_;
  std::shared_ptr<const Families> families_ = std::make_shared<const Families>();
};

} // namespace pbr
