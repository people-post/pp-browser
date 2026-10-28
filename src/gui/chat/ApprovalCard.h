#pragma once

#include "common/thread/ThreadRecordTypes.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "lib/ai/ParkedApproval.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/ToolPermissionPrompt.h"
#include "lib/ai/WorkingSetPolicy.h"
#include "lib/ui/ChatFormHelper.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * A stored permission card (assistant message, blocks JSON) after the request ended: the same message with its
 * buttons gone (stale notice, or the explanation plus one result line) and the bubble markup re-rendered.
 * nullopt when the stored text is not a card document or does not render.
 */
inline std::optional<ThreadMessage> ClosedApprovalCardMessage(ThreadMessage message, const ApprovalOutcome outcome) {
  const auto blocks = RewriteToolPermissionCardBlocks(message.text, outcome);
  if (!blocks) {
    return std::nullopt;
  }
  const ParseResult parsed = StructuredTextParser::ParseBlocksJson(*blocks, InferResponseGoalFromBlocksJson(*blocks));
  if (!parsed.ok) {
    return std::nullopt;
  }
  message.text = *blocks;
  message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", message.text) +
                        R"( selectable="text">)" + InjectEntryPlaceholders(parsed.rml, message.id) + "</div>";
  message.chat_actions.clear();
  message.working_set_json.reset();
  return message;
}

/** What a tap on a card that the agent refused (ResumeToolPermission returned an error) shows the user. */
enum class ApprovalTapReaction {
  Silent,       // already answered: the first tap's result closes the card
  StaleRewrite, // the request is gone: toast "no longer active" and turn the tapped card into that notice
  BusyElsewhere // the assistant is answering another chat: toast, the card stays tappable
};

/** Maps the refusal code (ApprovalResumeErrorCode) to a reaction; the raw code is never shown. */
inline ApprovalTapReaction ReactionToResumeError(const std::string& code) {
  if (code == ApprovalResumeErrorCode(ApprovalResumeError::AlreadyResolved)) {
    return ApprovalTapReaction::Silent;
  }
  if (code == ApprovalResumeErrorCode(ApprovalResumeError::TurnBusy)) {
    return ApprovalTapReaction::BusyElsewhere;
  }
  return ApprovalTapReaction::StaleRewrite; // not found / superseded / expired, and anything unexpected
}

/**
 * Rewrite one stored card by its id (no search of the thread; a card with any number of messages after it is
 * found the same way). `Store` is a thread store or the conversations facade. True when the stored message changed.
 */
template <class Store>
bool CloseApprovalCardInStore(Store& store, const std::string& thread_id, const std::string& entry_id,
                              const ApprovalOutcome outcome) {
  if (thread_id.empty() || entry_id.empty()) {
    return false;
  }
  auto found = store.GetMessage(thread_id, entry_id);
  if (!found || !found->has_value()) {
    return false;
  }
  auto closed = ClosedApprovalCardMessage(**found, outcome);
  if (!closed) {
    return false;
  }
  auto updated = store.UpdateMessage(*closed);
  return static_cast<bool>(updated);
}

} // namespace pbr
