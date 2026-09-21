#pragma once

#include "domain/messaging/CallTypes.h"
#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Shared N× pairwise DM fan-out (calls SoftMigrate/roster + group membership).
 * Pure iteration policy — callers own send/crypto/IO.
 */
enum class PairwiseFanoutMode {
  /** One peer failure does not block the rest (CallSfuAttach / roster). */
  BestEffort = 0,
  /** Stop on first send failure (group membership events). */
  FailFast = 1,
};

struct PairwiseFanoutResult {
  size_t attempted = 0;
  size_t succeeded = 0;
  std::vector<std::string> failed_identities;
  std::vector<std::string> failure_messages;
  /** Set when mode is FailFast and a send returned an error. */
  std::optional<pp::Error> first_error;
};

/** Skip empty / skip_identity; optional include predicate (nullptr = all remaining). */
std::vector<std::string> SelectPairwiseFanoutTargets(
    const std::vector<std::string>& identities, const std::string& skip_identity,
    const std::function<bool(const std::string& identity)>& include = {});

/**
 * Call-roster selectors for FanOutToJoined / FanOutToJoinedAndRinging.
 * `include_ringing_or_invited` adds Ringing + Invited (cancel/end clear path).
 */
std::vector<std::string> SelectCallFanoutIdentities(const std::vector<CallParticipant>& participants,
                                                   const std::string& skip_identity, bool include_joined,
                                                   bool include_ringing_or_invited);

/**
 * Invoke `send` once per target. BestEffort always returns ok(); FailFast returns first_error
 * via result (and stops). Callers that need Roe wrap FailFast themselves.
 */
PairwiseFanoutResult FanOutPairwise(
    const std::vector<std::string>& targets, PairwiseFanoutMode mode,
    const std::function<pp::Roe<void>(const std::string& identity)>& send);

} // namespace pbr
