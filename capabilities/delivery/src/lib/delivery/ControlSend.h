#pragma once

#include "common/Error.h"
#include "common/thread/ThreadTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** One recipient's message in a control-lane send to many: each is sealed for its own pair. */
struct ControlSend {
  std::string peer_identity;
  ThreadMessage control;
  /** A session init the caller holds for this pair (its media-key wrap opened the session); rides the send. */
  std::optional<std::string> key_init_b64;
  /** Wake the peer at once, the platform's most urgent push (an incoming call's). */
  bool urgent = false;
};

/** How one recipient's send went: queued (the outbox has it), or why not (no route, no session). */
struct ControlSendResult {
  std::string peer_identity;
  Roe<void> queued;
};

/** The results of a send to many, in the order of the sends. */
using ControlSendsDone = std::function<void(std::vector<ControlSendResult> results)>;

} // namespace pbr
