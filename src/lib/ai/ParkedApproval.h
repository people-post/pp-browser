#pragma once

#include "lib/ai/LlmClient.h"
#include "lib/ai/TurnPlan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

enum class AgentTurnMode { Conversation, Thread, ScopedAssist };

enum class ParkedApprovalState {
  Pending,
  Executing,
  Resolved,
  Cancelled,
};

/** Single in-flight tool-permission confirm per agent thread (v1). */
struct ParkedApproval {
  std::string id;
  std::string thread_id;
  std::string entry_id;
  /** Id of the stored assistant message that carries the permission card (empty until it is persisted). */
  std::string card_entry_id;
  TurnPlan plan;
  size_t next_tool_index = 0;
  std::vector<ChatMessage> scratch_so_far;
  std::vector<std::string> tools_executed;
  std::vector<PlannedToolCall> offered_tools;
  ParkedApprovalState state = ParkedApprovalState::Pending;
  int64_t created_at_ms = 0;
  AgentTurnMode turn_mode = AgentTurnMode::Conversation;
};

/** How a permission card ended; the GUI rewrites the stored card message to match. */
enum class ApprovalOutcome {
  Stale, // cancelled, superseded or expired: the card cannot be answered any more
  AllowOnce,
  AllowAlways,
  Deny,
};

enum class ApprovalResumeError {
  Ok,
  NotFound,
  AlreadyResolved,
  Superseded,
  Expired,
  ThreadMismatch,
  TurnBusy,
  InvalidDecision,
};

inline const char* ApprovalResumeErrorCode(ApprovalResumeError err) {
  switch (err) {
  case ApprovalResumeError::Ok:
    return "ok";
  case ApprovalResumeError::NotFound:
    return "approval_not_found";
  case ApprovalResumeError::AlreadyResolved:
    return "approval_already_resolved";
  case ApprovalResumeError::Superseded:
    return "approval_superseded";
  case ApprovalResumeError::Expired:
    return "approval_expired";
  case ApprovalResumeError::ThreadMismatch:
    return "approval_thread_mismatch";
  case ApprovalResumeError::TurnBusy:
    return "turn_busy";
  case ApprovalResumeError::InvalidDecision:
    return "approval_invalid_decision";
  }
  return "approval_error";
}

} // namespace pbr
