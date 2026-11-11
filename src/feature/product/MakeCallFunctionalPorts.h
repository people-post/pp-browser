#pragma once

#include "feature/calls/CallFunctionalPorts.h"
#include "feature/conversations/ConversationsHub.h"

namespace pbr {

class CallUiBackend;
class SessionStore;

/** Optional session_store wires profile `call_diagnostics` (CLI `--debug` still ORs in). */
CallFunctionalPorts MakeCallFunctionalPorts(CallUiBackend& backend, ConversationsHub& hub,
                                            SessionStore* session_store = nullptr);

/** The one grey line per caller for calls "who can call me" dropped (DEV/PLAN-call-stranger-declined.md). */
std::string BlockedCallLineId(const std::string& caller);
/** How many counted call ids the line keeps (a redelivered invite, after a restart too, is not counted again). */
inline constexpr size_t kBlockedCallIdsKept = 20;
/**
 * Count the dropped call `blocked` in its caller's line in `thread_id` (the line is made on the first): a local
 * system row (control_type `call_blocked`, words made by the transcript), never sent, no unread, never model history
 * (kLocalActionGeneration). Its time is this call's (`now_ms`; an expired invite's own time), never earlier than it
 * was. Returns the preview words to show with the chat moved up to now; none when the list stays as it is (this call
 * counted already, or an expired invite — its first write places the chat by the line's own time).
 */
Roe<std::optional<std::string>> RecordBlockedCallLine(IThreadStore& store, const std::string& thread_id,
                                                      const BlockedCallInvite& blocked, int64_t now_ms);

} // namespace pbr
