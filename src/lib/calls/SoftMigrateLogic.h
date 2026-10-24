#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/**
 * Why SoftMigrate was invoked (V021 who-picks).
 * Pure — no I/O; unit-tested in isolation.
 */
enum class SoftMigrateTrigger {
  /**
   * Peer received CallAccept and saw N≥3. First hop pick only if local is the
   * call initiator (sticky payer, V021/V022). Non-initiators WaitForAttach;
   * CallRoster notifies the initiator to SoftMigrate.
   */
  RemoteAcceptObserved = 0,
  /**
   * Local just joined an N≥3 call without an sfu_hint — wait for CallSfuAttach; never pick.
   */
  LocalJoinedWithoutHint = 1,
  /** ICE fail / hop re-pick — epoch coordinator only. */
  IceRecover = 2,
  /**
   * Joined count changed via CallRoster (or equivalent). Same first-pick rule as
   * RemoteAcceptObserved: initiator only when sfu_hint empty.
   */
  JoinedCountObserved = 3,
};

enum class SoftMigrateAction {
  /** Already on SFU / nothing to do. */
  NoOp = 0,
  /** Local should quote/attach and fan out CallSfuAttach. */
  PickHop = 1,
  /** Wait for CallSfuAttach from the picker (keep attach-wait). */
  WaitForAttach = 2,
};

struct SoftMigrateJoinedPeer {
  std::string identity;
  /** Unix ms when joined; missing → treated as later than any stamped peer. */
  std::optional<int64_t> joined_at;
};

/**
 * Sticky call initiator = earliest joined_at among Joined peers (V021/V022 payer). After the
 * initiator leaves, the same rule names the next owner of re-picks (V050) — every participant must
 * compute the same answer from its own rows, so the order never depends on row order: stamped
 * before unstamped, then earliest stamp, then smallest identity. Stamps come from one clock: the
 * inviter stamps each join and invites / CallRoster carry the stamps; an invitee never stamps from
 * its own clock (the store keeps the earliest stamp, so a local reading could win and diverge).
 */
std::string SelectCallInitiator(const std::vector<SoftMigrateJoinedPeer>& joined);

struct SoftMigrateDecisionInput {
  std::string local_identity;
  std::vector<std::string> joined_identities;
  /** Earliest-joined sticky initiator (SelectCallInitiator). */
  std::string initiator_identity;
  /** True when session has no usable sfu_hint yet. */
  bool sfu_hint_empty = true;
  SoftMigrateTrigger trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  bool already_on_sfu = false;
};

/**
 * Pure soft-migrate who-picks (V021 + V022).
 * First hop: call initiator only. Re-pick / ICE: epoch coordinator.
 */
SoftMigrateAction DecideSoftMigrate(const SoftMigrateDecisionInput& in);

// --- Around the who-picks rule ---------------------------------------------------------------

struct SoftMigrateArmingInput {
  /** No arming bound (harness): nothing gates. */
  bool arming_bound = false;
  size_t n_joined = 0;
  SoftMigrateTrigger trigger = SoftMigrateTrigger::RemoteAcceptObserved;
  /** A hop was named (a joiner's hop hint re-pick). */
  bool prefer_named = false;
  /** The call's Status lets a SoftMigrate arm from where it is (V037). */
  bool soft_migrate_may_arm = false;
  /** The hop planner already runs (migrate ops allowed). */
  bool migrate_ops_allowed = false;
};

enum class SoftMigrateArming {
  /** Go on; the planner is already armed (or nothing gates). */
  Proceed,
  /** Go on and arm the hop planner (Migrating). */
  ArmAndProceed,
  /** 1:1 stays Direct: done, nothing to migrate. */
  StayDirect,
  /** The hop planner is not armed and may not arm: fail. */
  NotArmed,
};

/**
 * V037/V048: a SoftMigrate from a Direct Status arms Migrating only for a group (N≥3), an ICE
 * recover or a named hop — a relay-cap nudge must not turn a connecting 1:1 into a group setup.
 */
SoftMigrateArming DecideSoftMigrateArming(const SoftMigrateArmingInput& in);

struct HopHostingFacts {
  /** This device may host the hop for the call (PreferLocal allowed for its scope, V035). */
  bool prefer_local_allowed = false;
  /** This device is configured to host hops at all. */
  bool hosts_hops = false;
  /** A durable media_relay Node is known. */
  bool durable_hop_known = false;
};

/**
 * Who hosts adjusts who-picks: a PreferLocal Node picks (itself) even where the rule says wait; a
 * phone defers its pick to a durable media_relay Node (its quote would hit a stranger refusal).
 */
SoftMigrateAction AdjustForHopHosting(SoftMigrateAction action, const HopHostingFacts& facts);

struct RepickOnHopInput {
  /** The hop media runs on now (attached, else the stored hint; empty: unknown). */
  std::string current_hop;
  /** The hop the re-pick names (empty: none). */
  std::string prefer_hop;
  /** This device's own hop peer id (empty: unknown). */
  std::string local_peer_id;
  /** This device hosts the hop the call runs on (PreferLocal). */
  bool local_hop_attached = false;
  /** The named hop can be dialed (dial book or a resolvable multiaddr). */
  bool prefer_dialable = false;
};

enum class RepickOnHopAction {
  /** Already on the named hop: re-fan-out its attach only (never detach and reattach). */
  StayOnNamedHop,
  /** Nothing named: keep the current hop, re-fan-out its attach. */
  KeepCurrentHop,
  /** Keep hosting it here: a re-pick only leaves PreferLocal for a different shared hop. */
  KeepPreferLocal,
  /** The named hop cannot be dialed: keep the current one. */
  KeepNotDialable,
  /** Detach and pick again, the named hop first. */
  Detach,
};

/** A re-pick while media runs on a hop (V035: same hop → re-fan-out only; hop-hint storms). */
RepickOnHopAction DecideRepickOnHop(const RepickOnHopInput& in);

enum class HopPickFailure {
  /** No hop was tried. */
  NoneTried,
  /** Every hop refused payment. */
  AllPayment,
  /** No hop could be dialed (a network hint helps). */
  AllUndialable,
  /** Mixed reasons. */
  Other,
};

/** What a failed pick tells the user, from each tried hop's failure text. */
HopPickFailure ClassifyHopPickFailures(const std::vector<std::string>& failures);

} // namespace pbr
