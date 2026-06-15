#include "demo/ChatDemo.h"

#include "agent/StructuredTextParser.h"
#include "agent/conversation/Conversation.h"
#include "app/Application.h"
#include "app/InputCoordinator.h"
#include "demo/CalendarHelper.h"
#include "demo/ChatFormHelper.h"
#include "demo/ChatWidgetStateBuilder.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"
#include "ui/SplitLayoutHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace pbr {

namespace {

std::string ToolActivityLabel(const std::string& tool_name, const std::string& status) {
  if (tool_name == "web_search") {
    if (status == "running") {
      return "Searching the web...";
    }
    if (status == "done") {
      return "Search complete";
    }
    if (status == "error") {
      return "Search failed";
    }
  }
  if (status == "running") {
    return "Running " + tool_name + "...";
  }
  if (status == "done") {
    return "Finished " + tool_name;
  }
  return tool_name;
}

std::string Trim(const std::string& text) {
  const auto start = std::find_if_not(text.begin(), text.end(), [](unsigned char c) { return std::isspace(c); });
  const auto end = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char c) { return std::isspace(c); }).base();
  if (start >= end) {
    return {};
  }
  return std::string(start, end);
}

std::string ToLower(std::string text) {
  for (char& c : text) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::optional<int> EventArgAsInt(const Rml::VariantList& args, size_t index) {
  if (args.size() <= index) {
    return std::nullopt;
  }
  const Rml::Variant& value = args[index];
  switch (value.GetType()) {
  case Rml::Variant::INT:
    return value.Get<int>();
  case Rml::Variant::INT64:
    return static_cast<int>(value.Get<int64_t>());
  case Rml::Variant::FLOAT:
    return static_cast<int>(value.Get<float>());
  case Rml::Variant::DOUBLE:
    return static_cast<int>(value.Get<double>());
  case Rml::Variant::STRING:
    try {
      return std::stoi(std::string(value.Get<Rml::String>().c_str()));
    } catch (...) {
      return std::nullopt;
    }
  default:
    return std::nullopt;
  }
}

Rml::String UserMessageRml(const std::string& text) {
  return Rml::String(("<div class=\"bubble bubble-user\" selectable=\"text\"><p>" + StructuredTextParser::EscapeText(text) +
                      "</p></div>")
                         .c_str());
}

Rml::String AssistantBubbleRml(const std::string& rml) {
  return Rml::String(rml.c_str());
}

std::string InlineChatActionButtonsRml(const std::vector<TranscriptChatAction>& chat_actions) {
  std::ostringstream out;
  for (size_t i = 0; i < chat_actions.size(); ++i) {
    out << "<button class=\"chat-suggestion\" data-event-click=\"send_chat_action('__ENTRY__', " << i << ")\">"
        << StructuredTextParser::EscapeText(chat_actions[i].label) << "</button>";
  }
  return out.str();
}

std::string HydrateLegacyChatActions(const std::string& assistant_rml,
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

Rml::String ErrorMessageRml(const std::string& message) {
  return Rml::String(("<p class=\"error\">" + StructuredTextParser::EscapeText(message) + "</p>").c_str());
}

std::string TruncatePreview(const std::string& text, size_t max_len = 48) {
  if (text.size() <= max_len) {
    return text;
  }
  return text.substr(0, max_len - 3) + "...";
}

std::string MockAssistantRespond(const std::string& query) {
  const std::string lower = ToLower(query);

  if (lower.find("help") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "heading", "level": 2, "text": "Help" },
        { "type": "paragraph", "text": "pp-browser renders structured JSON blocks with reactive widgets." },
        { "type": "list", "ordered": false, "items": [
          "Type a message and click Send",
          "Try help, list, code, button, form, calendar, card, or poll",
          "Press Escape to quit"
        ]}
      ]
    })JSON";
  }

  if (lower.find("list") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Here is an ordered list:" },
        { "type": "list", "ordered": true, "items": ["First item", "Second item", "Third item"] }
      ]
    })JSON";
  }

  if (lower.find("code") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Example code block:" },
        { "type": "code", "text": "auto result = StructuredTextParser::ParseBlocksJson(json);\nif (result.ok) { /* render */ }" }
      ]
    })JSON";
  }

  if (lower.find("form") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Fill out the booking form below and click Submit." },
        { "type": "form", "id": "booking", "title": "Booking form (mock)", "submit_label": "Submit",
          "submit_template": "Book for {{name}} on {{date}}",
          "fields": [
            { "id": "name", "label": "Name", "field_type": "text" },
            { "id": "date", "label": "Date", "field_type": "date" }
          ]
        }
      ]
    })JSON";
  }

  if (lower.find("calendar") != std::string::npos) {
    return MockCalendarReplyJson();
  }

  if (lower.find("card") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "card", "title": "Sample card", "subtitle": "Reactive templates", "variant": "highlight",
          "body": "Static card blocks render inline inside the assistant bubble." }
      ]
    })JSON";
  }

  if (lower.find("poll") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "poll", "question": "Which topic next?", "options": [
          { "label": "Calendar", "message": "Show me the calendar widget again." },
          { "label": "Forms", "message": "Show me the booking form again." }
        ]}
      ]
    })JSON";
  }

  if (lower.find("button") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Click a suggestion to send it as your next message:" },
        { "type": "button", "label": "Explain more", "message": "Can you explain that in simpler terms?" },
        { "type": "button", "label": "Give an example", "message": "Can you give a concrete example?" }
      ]
    })JSON";
  }

  return R"JSON({
    "blocks": [
      { "type": "paragraph", "text": "Thanks for your message. This is a mock assistant response." },
      { "type": "paragraph", "text": "Try help, list, code, button, form, calendar, card, or poll." }
    ]
  })JSON";
}

void DirtyChatChrome() {
  DataModelHost::Instance().Dirty("chat", "draft");
  DataModelHost::Instance().Dirty("chat", "status");
  DataModelHost::Instance().Dirty("chat", "loading");
}

void DirtyChatTurns() {
  DataModelHost::Instance().Dirty("chat", "turns");
}

void DirtyChat() {
  DirtyChatChrome();
  DirtyChatTurns();
}

void DirtyShell() {
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "preview_rml");
}

} // namespace

ChatDemo::ChatDemo() {
  redirectLogger("ChatDemo");
}

ChatDemo& ChatDemo::Instance() {
  static ChatDemo demo;
  return demo;
}

void ChatDemo::SendMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                   const Rml::VariantList& /*args*/) {
  Instance().OnSendMessage();
}

void ChatDemo::SendSuggestionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SendUserText(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatDemo::SubmitFormCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                  const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SubmitForm(std::string(args[0].Get<Rml::String>().c_str()),
                        std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatDemo::SendChatActionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }

  const std::optional<int> action_index = EventArgAsInt(args, 1);
  if (!action_index || *action_index < 0) {
    return;
  }

  Instance().SendChatAction(std::string(args[0].Get<Rml::String>().c_str()), *action_index);
}

void ChatDemo::CalendarPrevCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarPrev(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatDemo::CalendarNextCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarNext(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatDemo::SelectCalendarDayCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                         const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SelectCalendarDay(std::string(args[0].Get<Rml::String>().c_str()),
                               std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatDemo::NewChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  Instance().OnNewChat();
}

void ChatDemo::OnSendMessage() {
  if (chat_.loading) {
    return;
  }

  const std::string text = Trim(chat_.draft.c_str());
  if (text.empty()) {
    return;
  }

  chat_.draft = "";
  DirtyChatChrome();
  SendUserText(text);
}

void ChatDemo::OnNewChat() {
  if (agent_) {
    agent_->StartNewConversation();
  }
  chat_.draft = "";
  chat_.status = "";
  chat_.loading = false;
  chat_.turns.clear();
  shell_.preview_rml = "";
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  ClearFormState();
  widgets_by_entry_.clear();
  DirtyChat();
  DirtyShell();
}

bool ChatDemo::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  if (submitted_forms_.count({entry_id, form_id}) > 0) {
    return false;
  }
  return active_form_ && active_form_->entry_id == entry_id && active_form_->form_id == form_id;
}

TurnWidgetState* ChatDemo::FindWidgetState(const std::string& entry_id) {
  const auto it = widgets_by_entry_.find(entry_id);
  return it == widgets_by_entry_.end() ? nullptr : &it->second;
}

const TurnWidgetState* ChatDemo::FindWidgetState(const std::string& entry_id) const {
  const auto it = widgets_by_entry_.find(entry_id);
  return it == widgets_by_entry_.end() ? nullptr : &it->second;
}

void ChatDemo::MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
  const TurnWidgetState* widgets = FindWidgetState(entry_id);
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

std::string ChatDemo::HydrateAssistantRml(const TranscriptEntry& entry) const {
  if (!entry.assistant_rml) {
    return {};
  }

  std::string rml = HydrateLegacyChatActions(*entry.assistant_rml, entry.chat_actions);
  return InjectEntryPlaceholders(rml, entry.id);
}

void ChatDemo::InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
  TurnWidgetState state;
  ApplyWidgetInits(inits, state);
  widgets_by_entry_[entry_id] = std::move(state);

  if (const TurnWidgetState* widgets = FindWidgetState(entry_id); widgets && widgets->has_form) {
    const std::string form_id = std::string(widgets->form.form_id.c_str());
    if (submitted_forms_.count({entry_id, form_id}) == 0) {
      active_form_ = ActiveForm{.entry_id = entry_id, .form_id = form_id};
      ExpireFormsExcept(entry_id, form_id);
    }
  }
}

void ChatDemo::ExpireFormsExcept(const std::string& entry_id, const std::string& form_id) {
  for (auto& [id, widgets] : widgets_by_entry_) {
    if (!widgets.has_form) {
      continue;
    }
    const std::string widget_form_id = std::string(widgets.form.form_id.c_str());
    if (id != entry_id || widget_form_id != form_id) {
      widgets.form.expired = true;
    }
  }
}

void ChatDemo::ClearFormState() {
  active_form_.reset();
  submitted_forms_.clear();
}

void ChatDemo::SyncDisplayFromConversation() {
  if (!agent_) {
    return;
  }

  chat_.turns.clear();
  for (const TranscriptEntry& entry : agent_->conversation().Entries()) {
    TranscriptDisplayRow row;
    row.user_content_rml = UserMessageRml(entry.user_text);
    if (entry.assistant_rml) {
      row.assistant_content_rml = AssistantBubbleRml(HydrateAssistantRml(entry));
      row.has_assistant = true;
      MergeWidgetStateIntoRow(entry.id, row);
    } else if (entry.assistant_raw && !entry.assistant_rml) {
      row.assistant_content_rml = ErrorMessageRml("Assistant reply pending display sync.");
      row.has_assistant = true;
    }
    chat_.turns.push_back(std::move(row));
  }

  DirtyChatTurns();
}

void ChatDemo::UpdateSidebarPreview(const std::string& preview_text) {
  if (shell_.sessions.empty()) {
    shell_.sessions.push_back({Rml::String("Chat"), Rml::String("Ask anything...")});
  }
  shell_.sessions[0].preview = Rml::String(TruncatePreview(preview_text).c_str());
  DirtyShell();
}

void ChatDemo::SendUserText(const std::string& text, std::optional<std::string> user_payload) {
  const std::string trimmed = Trim(text);
  if (trimmed.empty() || chat_.loading) {
    return;
  }

  for (auto& [id, widgets] : widgets_by_entry_) {
    if (widgets.has_form && !widgets.form.expired) {
      widgets.form.expired = true;
    }
  }
  active_form_.reset();
  DirtyChatTurns();

  chat_.loading = true;
  chat_.status = "";
  UpdateSidebarPreview(trimmed);
  DirtyChatChrome();

  if (!use_llm_) {
    if (!agent_) {
      return;
    }
    TranscriptEntry& entry = agent_->AppendUserMessage(trimmed, std::move(user_payload));
    SyncDisplayFromConversation();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = entry.id, .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  log().info << "Submitting message to agent session";
  agent_->Submit(trimmed, std::move(user_payload));
}

void ChatDemo::SubmitForm(const std::string& entry_id, const std::string& form_id) {
  if (chat_.loading) {
    return;
  }
  if (!IsFormEditable(entry_id, form_id)) {
    log().warning << "Ignoring submit for inactive or expired form: " << entry_id << "/" << form_id;
    return;
  }

  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_form) {
    log().warning << "Form widget state missing: " << entry_id << "/" << form_id;
    return;
  }

  const std::string bound_form_id = std::string(widgets->form.form_id.c_str());
  if (bound_form_id != form_id) {
    log().warning << "Form id mismatch: " << bound_form_id << " vs " << form_id;
    return;
  }

  const std::map<std::string, std::string> values = FormValuesMap(widgets->form);
  const std::string display_text =
      ApplySubmitTemplate(std::string(widgets->form.submit_template.c_str()), values);
  const std::string payload = BuildFormSubmissionPayload(form_id, values);

  submitted_forms_.insert({entry_id, form_id});
  widgets->form.expired = true;
  active_form_.reset();

  SyncDisplayFromConversation();
  SendUserText(display_text, payload);
}

void ChatDemo::SendChatAction(const std::string& entry_id, int action_index) {
  if (!agent_ || chat_.loading || action_index < 0) {
    return;
  }

  for (const TranscriptEntry& entry : agent_->conversation().Entries()) {
    if (entry.id != entry_id) {
      continue;
    }
    if (action_index >= static_cast<int>(entry.chat_actions.size())) {
      log().warning << "Chat action index out of range: " << entry_id << "/" << action_index;
      return;
    }
    const TranscriptChatAction& action = entry.chat_actions[static_cast<size_t>(action_index)];
    SendUserText(action.message, action.payload);
    return;
  }

  log().warning << "Chat action entry not found: " << entry_id;
}

void ChatDemo::CalendarPrev(const std::string& entry_id) {
  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }
  ShiftCalendarMonth(widgets->calendar, -1);
  SyncDisplayFromConversation();
}

void ChatDemo::CalendarNext(const std::string& entry_id) {
  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }
  ShiftCalendarMonth(widgets->calendar, 1);
  SyncDisplayFromConversation();
}

void ChatDemo::SelectCalendarDay(const std::string& entry_id, const std::string& iso_date) {
  if (chat_.loading) {
    return;
  }
  const TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }

  bool available = false;
  for (const CalendarWeekRow& week : widgets->calendar.weeks) {
    for (const CalendarDayRow& day : week.days) {
      if (std::string(day.iso_date.c_str()) == iso_date && day.available) {
        available = true;
        break;
      }
    }
    if (available) {
      break;
    }
  }
  if (!available) {
    return;
  }

  SendUserText("Selected " + iso_date);
}

void ChatDemo::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                                    const std::string& finish_reason) {
  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output)
                         : StructuredTextParser::ParseBlocksJson(raw_output);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    log().warning << "AI response: " << raw_output;
    if (agent_) {
      if (!from_llm) {
        agent_->CompleteAssistantMessage(entry_id, raw_output);
      }
      agent_->SetAssistantDisplay(entry_id, parsed.error, {});
    }
  } else {
    for (const std::string& warning : parsed.warnings) {
      log().warning << "Skipped block in assistant reply: " << warning;
    }

    if (!parsed.widget_inits.empty()) {
      InitializeWidgetState(entry_id, parsed.widget_inits);
    }

    std::vector<TranscriptChatAction> chat_actions;
    chat_actions.reserve(parsed.chat_actions.size());
    for (const ParsedChatAction& action : parsed.chat_actions) {
      chat_actions.push_back({action.label, action.message, action.payload});
    }
    if (agent_) {
      if (!from_llm) {
        agent_->CompleteAssistantMessage(entry_id, raw_output);
      }
      agent_->SetAssistantDisplay(entry_id, parsed.rml, std::move(chat_actions));
    }
    shell_.preview_rml = Rml::String(parsed.rml.c_str());
    DirtyShell();
  }

  SyncDisplayFromConversation();
  chat_.loading = false;
  chat_.status = "";
  DirtyChatChrome();
}

void ChatDemo::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      chat_.status = "";
    }
    DirtyChatChrome();
    break;
  case AgentEventType::ToolActivity:
    chat_.status = Rml::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    DirtyChatChrome();
    break;
  case AgentEventType::AssistantReady:
    FinishAssistantReply(event.entry_id, event.text, true, event.finish_reason);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    if (agent_ && !agent_->conversation().Entries().empty()) {
      const TranscriptEntry& entry = agent_->conversation().Entries().back();
      agent_->CompleteAssistantMessage(entry.id, event.message);
      agent_->SetAssistantDisplay(entry.id, event.message, {});
      SyncDisplayFromConversation();
    }
    chat_.loading = false;
    chat_.status = "";
    DirtyChatChrome();
    break;
  }
}

bool ChatDemo::Setup(Rml::Context* context, const AppConfig& config) {
  if (!context) {
    return false;
  }

  context_ = context;
  ClearFormState();
  widgets_by_entry_.clear();
  chat_ = {};
  shell_ = {};
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  agent_.emplace();
  agent_->Configure(config);
  log().info << "Chat demo initialized (model: " << config.llm.model << ")";

  DataModelHost::Instance().Clear();

  const auto register_enter_send = [this](Rml::Input::KeyIdentifier key) {
    InputCoordinator::Instance().Register(KeyBinding{
        .key = key,
        .forbidden_modifiers = Rml::Input::KM_SHIFT,
        .when = [](Rml::Context* ctx) {
          Rml::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
          return focus && focus->GetId() == "draft-input";
        },
        .action = [this]() {
          OnSendMessage();
          return false;
        },
        .priority = 50,
    });
  };
  register_enter_send(Rml::Input::KI_RETURN);
  register_enter_send(Rml::Input::KI_NUMPADENTER);

  if (!DataModelHost::Instance().Register(context, "chat", [](Rml::DataModelConstructor& ctor) {
        RegisterChatWidgetDataTypes(ctor);
        ctor.Bind("draft", &ChatDemo::Instance().chat_.draft);
        ctor.Bind("status", &ChatDemo::Instance().chat_.status);
        ctor.Bind("loading", &ChatDemo::Instance().chat_.loading);
        ctor.Bind("turns", &ChatDemo::Instance().chat_.turns);
        ctor.BindEventCallback("send_message", &ChatDemo::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatDemo::SendSuggestionCallback);
        ctor.BindEventCallback("send_chat_action", &ChatDemo::SendChatActionCallback);
        ctor.BindEventCallback("submit_form", &ChatDemo::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatDemo::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatDemo::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatDemo::SelectCalendarDayCallback);
      })) {
    return false;
  }

  if (!DataModelHost::Instance().Register(context, "shell", [](Rml::DataModelConstructor& ctor) {
        if (auto session_handle = ctor.RegisterStruct<ChatDemo::SessionRow>()) {
          session_handle.RegisterMember("title", &ChatDemo::SessionRow::title);
          session_handle.RegisterMember("preview", &ChatDemo::SessionRow::preview);
        }
        ctor.RegisterArray<std::vector<ChatDemo::SessionRow>>();
        ctor.Bind("sessions", &ChatDemo::Instance().shell_.sessions);
        ctor.Bind("preview_rml", &ChatDemo::Instance().shell_.preview_rml);
        ctor.BindEventCallback("new_chat", &ChatDemo::NewChatCallback);
        ctor.BindEventCallback("split_panel_h", &SplitLayoutHost::SplitPanelHCallback);
        ctor.BindEventCallback("split_panel_v", &SplitLayoutHost::SplitPanelVCallback);
        ctor.BindEventCallback("close_panel", &SplitLayoutHost::ClosePanelCallback);
        ctor.BindEventCallback("gutter_drag_start", &SplitLayoutHost::GutterDragStartCallback);
        ctor.BindEventCallback("gutter_drag_end", &SplitLayoutHost::GutterDragEndCallback);
      })) {
    return false;
  }

  SplitLayoutHost::Instance().Initialize(context);

  if (DocumentLoader::LoadFile(context, Application::AssetsPath("samples/chat_shell.rml")) == nullptr) {
    return false;
  }

  SplitLayoutHost::Instance().SyncLayout();
  return true;
}

void ChatDemo::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm);
  }

  if (!agent_) {
    return;
  }

  std::vector<AgentEvent> events;
  agent_->PollEvents(events);
  for (const AgentEvent& event : events) {
    HandleAgentEvent(event);
  }
}

void ChatDemo::Shutdown() {
  if (agent_) {
    agent_->Cancel();
    agent_.reset();
  }
  pending_reply_.reset();
  context_ = nullptr;
  ClearFormState();
  widgets_by_entry_.clear();
  chat_ = {};
  shell_ = {};
  use_llm_ = false;
}

bool SetupChatDemo(Rml::Context* context, const AppConfig& config) {
  return ChatDemo::Instance().Setup(context, config);
}

void UpdateChatDemo() {
  ChatDemo::Instance().Update();
}

void ShutdownChatDemo() {
  ChatDemo::Instance().Shutdown();
}

} // namespace pbr
