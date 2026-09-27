#pragma once

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
 * Sticky call initiator = earliest joined_at among Joined peers (V021/V022 payer).
 * Ties / missing stamps: first identity in list order among candidates without stamps.
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

} // namespace pbr
