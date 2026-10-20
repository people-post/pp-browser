#pragma once

#include "common/chat/SystemControl.h"
#include "domain/messaging/CallTypes.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** What the call-control family reads from the call stack while bound. */
struct CallControlFamilyPorts {
  /** Hand an inbound call-control message to the calls owner (fire and forget: idempotent there). */
  std::function<void(const ThreadMessage& message, const SystemControlContext& context)> receive;
  std::function<bool()> has_active_local_call;
  /** The active local call's origin thread, when it has one. */
  std::function<std::optional<std::string>()> active_call_origin_thread_id;
};

/**
 * Call control as a system-control family: messaging carries it end to end and routes it here.
 * Applied out of order (media keys, accepts and the like are idempotent); plumbing and mid-call
 * roster updates are never shown; history lines (invite, joined, declined, left, started, ended)
 * are worded here, with names.
 */
SystemControlFamily MakeCallControlFamily(CallControlFamilyPorts ports);

/** The transcript line of a call-control message; none when it is not shown. */
std::optional<std::string> CallHistoryLine(const ThreadMessage& message, CallControlType type,
                                           const SystemControlLabels& labels);

} // namespace pbr
