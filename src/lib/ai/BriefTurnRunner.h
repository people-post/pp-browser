#pragma once

#include "common/Error.h"
#include "lib/ai/BriefAiClient.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace pbr {

using BriefAiStreamFn = std::function<Roe<BriefAiOutcome>(
    const BriefAiRequest&, const std::function<void(const BriefAiEvent&)>&, const std::atomic<bool>& cancel)>;

// Exactly one of on_done / on_handoff / on_error / on_cancelled fires per Run.
struct BriefTurnSinks {
  std::function<void(const std::string& route)> on_meta; // optional
  std::function<void(const std::string& text_so_far)> on_delta; // accumulated text after each token
  std::function<void(const std::string& tool, const std::string& phase, const std::string& query)> on_status;
  /** `role`: the role id that answered (empty when the server sent none). */
  std::function<void(const std::string& response, const std::string& finish, const std::vector<BriefAiSource>& sources,
                     const std::string& role)>
      on_done;
  /**
   * `capability`: the tool brief_AI handed the turn back for (empty when the server sent none). `reason`: why
   * ("app_action", "user_confirmed"; empty when absent).
   */
  std::function<void(const std::string& capability, const std::string& reason)> on_handoff;
  std::function<void(const std::string& message, bool retryable, const std::string& partial_text)> on_error;
  std::function<void(const std::string& partial_text)> on_cancelled;
};

// Drives one brief_AI stream and turns its events into sink calls (no UI, no stores).
class BriefTurnRunner {
public:
  /** How long a turn waits before asking a busy brief_AI (code server_busy) again. */
  static constexpr std::chrono::milliseconds kServerBusyRetryWait{3000};

  /**
   * Runs synchronously on the calling thread. Never logs message content.
   * brief_AI busy (HTTP 429 code server_busy, or an `error` event with that code before any token): waits
   * `server_busy_wait` (a cancel ends the wait) and asks once more within the same turn. A per-user limit
   * (user_concurrency, rate_limited, a 429 without a code) is never retried.
   */
  static void Run(const BriefAiStreamFn& stream, const BriefAiRequest& request, const std::atomic<bool>& cancel,
                  const BriefTurnSinks& sinks, std::chrono::milliseconds server_busy_wait = kServerBusyRetryWait);

private:
  enum class Attempt { Finished, ServerBusy };
  /**
   * One stream. ServerBusy (only when `may_retry`): nothing reached a terminal sink; ask again. `shown`: the step
   * events (meta, status) the busy first attempt passed to the sinks; the retry does not pass the same ones again.
   */
  static Attempt RunOnce(const BriefAiStreamFn& stream, const BriefAiRequest& request, const std::atomic<bool>& cancel,
                         const BriefTurnSinks& sinks, bool may_retry, std::multiset<std::string>& shown);
  static void LogBusy(std::chrono::milliseconds wait);
};

} // namespace pbr
