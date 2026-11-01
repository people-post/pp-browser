#pragma once

#include "common/chat/SystemControl.h"
#include "lib/calls/CallTypes.h"

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

/**
 * A call summary as the transcript draws it (2026-10-23): an icon for voice / video, the duration
 * or a one-word outcome, on the starter's side of the chat like a message. `ours` = this user
 * started the call (the row sits right); `connected` = it had a duration (else drawn muted). The
 * words are the app's (capability-repos CR015): the row says which outcome, the GUI says it.
 */
struct CallSummaryRow {
  /** The one-word outcome when there is no duration. */
  enum class Outcome { Connected, NoAnswer, Missed, Cancelled, Declined, DeclinedByPeer, Failed };
  bool ours = false;
  bool video = false;
  bool connected = false;
  /** A group call (the row leads with that). */
  bool group = false;
  Outcome outcome = Outcome::Failed;
  /** "m:ss" / "h:mm:ss" when connected; empty otherwise. */
  std::string duration;
};
std::optional<CallSummaryRow> CallSummaryRowOf(const ThreadMessage& message);

/** A summary the reader should hear about (unread, notice): a call to them that they missed. */
bool CallSummaryIsNews(const ThreadMessage& message);

} // namespace pbr
