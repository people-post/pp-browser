#pragma once

#include "common/chat/SystemControl.h"
#include "domain/calls/CallTypes.h"

#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** What the call-control family reads from the call stack while bound. */
struct CallControlFamilyPorts {
  /** Hand an inbound call-control message to the calls owner (fire and forget). */
  std::function<void(const ThreadMessage& message, const SystemControlContext& context)> receive;
  std::function<bool()> has_active_local_call;
};

/**
 * Call control as a system-control family. Signaling (invite, accept, media keys, SDP / ICE, hop
 * moves, roster …) rides the control lane: applied once while fresh, never a transcript row. The
 * call's summary — one per call, sent by its starter into the call's thread — is the only call
 * message in the chat stream; its line is worded here, from the reader's side.
 */
SystemControlFamily MakeCallControlFamily(CallControlFamilyPorts ports);

/** A call summary's transcript line; none when the message is no summary. */
std::optional<std::string> CallSummaryLine(const ThreadMessage& message, const SystemControlLabels& labels);

/** A summary the reader should hear about (unread, notice): a call to them that they missed. */
bool CallSummaryIsNews(const ThreadMessage& message);

} // namespace pbr
