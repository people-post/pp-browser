#include "feature/chat/ChatWidgetHost.h"

#include "domain/ai/StructuredTextParser.h"
#include "domain/ui/ChatFormHelper.h"
#include "feature/chat/CalendarHelper.h"
#include "feature/chat/ChatWidgetStateBuilder.h"

#include <sstream>

namespace pbr {
namespace {

std::string InlineChatActionButtonsRml(const std::vector<TranscriptChatAction>& chat_actions) {
  std::ostringstream out;
  for (size_t i = 0; i < chat_actions.size(); ++i) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " << i << ")\">"
        << StructuredTextParser::EscapeText(chat_actions[i].label) << "</button>";
  }
  return out.str();
}

} // namespace

std::string HydrateChatActionButtons(const std::string& assistant_rml,
                                     const std::vector<TranscriptChatAction>& chat_actions) {
  if (chat_actions.empty() || assistant_rml.find("chat-suggestion") != std::string::npos) {
    return assistant_rml;
  }

  const std::string buttons = InlineChatActionButtonsRml(chat_actions);
  constexpr const char* stack_close = "</div>";
  if (assistant_rml.size() > 6 && assistant_rml.find("<div class=\"stack\">") == 0 &&
      assistant_rml.compare(assistant_rml.size() - 6, 6, stack_close) == 0) {
    return assistant_rml.substr(0, assistant_rml.size() - 6) + buttons + stack_close;
  }
  return assistant_rml + buttons;
}

void ChatWidgetHost::ClearForms() {
  active_form_.reset();
  submitted_forms_.clear();
}

void ChatWidgetHost::ClearAll() {
  by_entry_.clear();
  ClearForms();
}

void ChatWidgetHost::ExpireOpenForms() {
  for (auto& [id, widgets] : by_entry_) {
    if (widgets.has_form && !widgets.form.expired) {
      widgets.form.expired = true;
    }
  }
  active_form_.reset();
}

TurnWidgetState* ChatWidgetHost::Find(const std::string& entry_id) {
  const auto it = by_entry_.find(entry_id);
  return it == by_entry_.end() ? nullptr : &it->second;
}

const TurnWidgetState* ChatWidgetHost::Find(const std::string& entry_id) const {
  const auto it = by_entry_.find(entry_id);
  return it == by_entry_.end() ? nullptr : &it->second;
}

bool ChatWidgetHost::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  if (submitted_forms_.count({entry_id, form_id}) > 0) {
    return false;
  }
  return active_form_ && active_form_->entry_id == entry_id && active_form_->form_id == form_id;
}

void ChatWidgetHost::MergeIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
  const TurnWidgetState* widgets = Find(entry_id);
  if (!widgets) {
    return;
  }

  if (widgets->has_form) {
    row.has_form = true;
    row.form = widgets->form;
    if (!IsFormEditable(entry_id, std::string(widgets->form.form_id.c_str()))) {
      row.form.expired = true;
    }
  }

  if (widgets->has_calendar) {
    row.has_calendar = true;
    row.calendar = widgets->calendar;
  }
}

std::string ChatWidgetHost::HydrateAssistantRml(const TranscriptEntry& entry) const {
  if (!entry.assistant_rml) {
    return {};
  }

  std::string rml = HydrateChatActionButtons(*entry.assistant_rml, entry.chat_actions);
  return InjectEntryPlaceholders(rml, entry.id);
}

void ChatWidgetHost::ExpireFormsExcept(const std::string& entry_id, const std::string& form_id) {
  for (auto& [id, widgets] : by_entry_) {
    if (!widgets.has_form) {
      continue;
    }
    const std::string widget_form_id = std::string(widgets.form.form_id.c_str());
    if (id != entry_id || widget_form_id != form_id) {
      widgets.form.expired = true;
    }
  }
}

void ChatWidgetHost::Initialize(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
  TurnWidgetState state;
  ApplyWidgetInits(inits, state);
  by_entry_[entry_id] = std::move(state);

  if (const TurnWidgetState* widgets = Find(entry_id); widgets && widgets->has_form) {
    const std::string form_id = std::string(widgets->form.form_id.c_str());
    if (submitted_forms_.count({entry_id, form_id}) == 0) {
      active_form_ = ActiveForm{.entry_id = entry_id, .form_id = form_id};
      ExpireFormsExcept(entry_id, form_id);
    }
  }
}

std::optional<ChatWidgetHost::FormSubmission> ChatWidgetHost::TrySubmit(const std::string& entry_id,
                                                                        const std::string& form_id) {
  if (!IsFormEditable(entry_id, form_id)) {
    return std::nullopt;
  }

  TurnWidgetState* widgets = Find(entry_id);
  if (!widgets || !widgets->has_form) {
    return std::nullopt;
  }

  const std::string bound_form_id = std::string(widgets->form.form_id.c_str());
  if (bound_form_id != form_id) {
    return std::nullopt;
  }

  const std::map<std::string, std::string> values = FormValuesMap(widgets->form);
  FormSubmission submission;
  submission.display_text =
      ApplySubmitTemplate(std::string(widgets->form.submit_template.c_str()), values);
  submission.payload = BuildFormSubmissionPayload(form_id, values);

  submitted_forms_.insert({entry_id, form_id});
  widgets->form.expired = true;
  active_form_.reset();
  return submission;
}

bool ChatWidgetHost::ShiftCalendar(const std::string& entry_id, const int delta_months) {
  TurnWidgetState* widgets = Find(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return false;
  }
  ShiftCalendarMonth(widgets->calendar, delta_months);
  return true;
}

bool ChatWidgetHost::IsCalendarDayAvailable(const std::string& entry_id,
                                            const std::string& iso_date) const {
  const TurnWidgetState* widgets = Find(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return false;
  }

  for (const CalendarWeekRow& week : widgets->calendar.weeks) {
    for (const CalendarDayRow& day : week.days) {
      if (std::string(day.iso_date.c_str()) == iso_date && day.available) {
        return true;
      }
    }
  }
  return false;
}

} // namespace pbr
