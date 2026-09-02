#pragma once

#include "base/ai/StructuredTextParser.h"
#include "base/ai/conversation/ConversationTypes.h"
#include "common/chat/ChatActionTypes.h"
#include "base/ui/ChatWidgetTypes.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

/** Append legacy suggestion buttons when RML lacks chat-suggestion markup. */
std::string HydrateChatActionButtons(const std::string& assistant_rml,
                                     const std::vector<TranscriptChatAction>& chat_actions);

/** Form / calendar widget state and hydrate helpers for chat turns. */
class ChatWidgetHost {
public:
  struct ActiveForm {
    std::string entry_id;
    std::string form_id;
  };

  struct FormSubmission {
    std::string display_text;
    std::string payload;
  };

  void ClearForms();
  /** Clear forms and forget all per-entry widget state. */
  void ClearAll();
  /** Mark any open forms expired (new user text supersedes). */
  void ExpireOpenForms();

  TurnWidgetState* Find(const std::string& entry_id);
  const TurnWidgetState* Find(const std::string& entry_id) const;

  void Initialize(const std::string& entry_id, const std::vector<WidgetInit>& inits);
  void MergeIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const;
  std::string HydrateAssistantRml(const TranscriptEntry& entry) const;

  bool IsFormEditable(const std::string& entry_id, const std::string& form_id) const;
  /** Validates and marks form submitted; returns text/payload for SendUserText. */
  std::optional<FormSubmission> TrySubmit(const std::string& entry_id, const std::string& form_id);
  bool ShiftCalendar(const std::string& entry_id, int delta_months);
  bool IsCalendarDayAvailable(const std::string& entry_id, const std::string& iso_date) const;

private:
  void ExpireFormsExcept(const std::string& entry_id, const std::string& form_id);

  std::map<std::string, TurnWidgetState> by_entry_;
  std::optional<ActiveForm> active_form_;
  std::set<std::pair<std::string, std::string>> submitted_forms_;
};

} // namespace pbr
