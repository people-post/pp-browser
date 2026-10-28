#pragma once

#include "common/Error.h"
#include "lib/ai/BriefAiClient.h"

#include <atomic>
#include <functional>
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
  std::function<void(const std::string& response, const std::string& finish, const std::vector<BriefAiSource>& sources)>
      on_done;
  /** `capability`: the tool brief_AI handed the turn back for (empty when the server sent none). */
  std::function<void(const std::string& capability)> on_handoff;
  std::function<void(const std::string& message, bool retryable, const std::string& partial_text)> on_error;
  std::function<void(const std::string& partial_text)> on_cancelled;
};

// Drives one brief_AI stream and turns its events into sink calls (no UI, no stores).
class BriefTurnRunner {
public:
  // Runs synchronously on the calling thread. Never logs message content.
  static void Run(const BriefAiStreamFn& stream, const BriefAiRequest& request, const std::atomic<bool>& cancel,
                  const BriefTurnSinks& sinks);
};

} // namespace pbr
