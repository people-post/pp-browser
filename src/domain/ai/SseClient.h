#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/ai/SseParser.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <functional>
#include <string>

namespace pbr {

struct SseRequest {
  std::string url;
  std::string bearer_token; // empty = no Authorization header
  std::string json_body;
  long connect_timeout_s = 10;
  long idle_timeout_s = 60; // abort when no bytes arrive for this long
};

enum class SseOutcome { Completed, Cancelled };

class SseClient : public Module {
public:
  SseClient();

  // Blocks until the stream ends. `on_event` runs on the calling thread.
  // `cancel` is polled while waiting; once true the connection is dropped promptly.
  Roe<SseOutcome> Post(const SseRequest& request, const std::function<void(const SseEvent&)>& on_event,
                       const std::atomic<bool>& cancel) const;
};

} // namespace pbr
