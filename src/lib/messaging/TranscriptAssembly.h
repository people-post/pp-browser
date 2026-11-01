#pragma once

#include "common/chat/SystemControl.h"
#include "common/thread/ThreadTypes.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** One emoji on a message: how many people reacted with it, and whether we did. */
struct TranscriptReaction {
  std::string emoji;
  size_t count = 0;
  bool mine = false;
};

/** One transcript row, before it is drawn. */
struct TranscriptRow {
  ThreadMessage message;
  /** An annotation whose target is not in the transcript: shown on its own, unlinked. */
  bool orphan_annotation = false;
  /** A feature's control message: its line carries the names (no sender chip, no transport badge). */
  bool control = false;
  /** An AI answer: the "@ai" question it answers, shown on top like a quote. */
  std::optional<std::string> asked_question;
  /** Reactions (D098: each person's latest per emoji; cleared ones gone). */
  std::vector<TranscriptReaction> reactions;
  /** Other annotations on the message, as their text. */
  std::vector<std::string> annotation_badges;
};

/**
 * The transcript `messages` (oldest first) show: which rows appear — never lane signaling, never
 * what hangs off a message deleted here, never a retract — with their reactions and badges folded
 * onto their targets (at most kMaxAnnotationsPerTarget each), the asked question on each AI
 * answer, and unlinked annotations as their own rows; all in display order. Drawing is the GUI's.
 * `thread_kind`: in a direct chat a group's membership events (stored there because they travel as direct
 * messages) are not shown — only the group invitation is; `group_dissolved` (G016) shows as a line in the group's own
 * chat only: the DM that carried it is not read.
 */
std::vector<TranscriptRow> AssembleTranscript(const std::vector<ThreadMessage>& messages,
                                                const SystemControlRegistry* controls,
                                                ThreadKind thread_kind = ThreadKind::Group);

} // namespace pbr
