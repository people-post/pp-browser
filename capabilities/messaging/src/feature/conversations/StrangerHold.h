#pragma once

#include "common/thread/IThreadStore.h"

#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The "held" state of a 1:1 chat (B236): the recipient has not added us, the relay refused a send of ours for its
 * stranger quota, and that message — the probe — is still queued (tried again by the outbox; sent at once if the
 * recipient's grant arrives). While held, the chat sends nothing more and shows one line saying why. It ends when the
 * probe is no longer queued: accepted (the grant came, or the quota refreshed) or failed for good (then the next send
 * may try again). Read from the store alone, so it survives leaving the chat and a restart: a stored "held" line
 * (MakeStrangerHeldRow) whose probe is still pending.
 */

/** Whether `line` (a stored "held" line) holds its chat now, its probe as stored (`probe`, none when gone). Pure. */
bool StrangerHoldActive(const ThreadMessage& line, const std::optional<ThreadMessage>& probe);

/**
 * The probe holding `thread_id` now, if any: among the store's pending outbox rows of that chat, one with a "held"
 * line. Blocking (the store).
 */
std::optional<std::string> ActiveStrangerHold(const IThreadStore& store, const std::string& thread_id);

/** Whether the "held" line `line` is shown: only while it holds its chat (StrangerHoldActive). Blocking (the store). */
bool StrangerHeldLineShown(const IThreadStore& store, const ThreadMessage& line);

} // namespace pbr
