#include "demo/ChatDemo.h"

#include "agent/StructuredTextParser.h"
#include "agent/conversation/Conversation.h"
#include "app/Application.h"
#include "app/InputCoordinator.h"
#include "demo/CalendarHelper.h"
#include "demo/ChatFormHelper.h"
#include "demo/ChatWidgetStateBuilder.h"
#include "messaging/IdUtil.h"
#include "messaging/MessagingHub.h"
#include "messaging/MessagingJson.h"
#include "messaging/ThreadTypes.h"
#include "ui/DataModelHost.h"
#include "ui/DocumentLoader.h"
#include "ui/ShellHost.h"
#include "ui/ShellFeedback.h"
#include "ui/ShellTypes.h"

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

  if (lower.find("find someone") != std::string::npos || lower.find("search people") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Here are people on the network (mock directory):" },
        { "type": "long_list", "title": "Search results", "items": [
          { "title": "Alice Example", "subtitle": "@alice", "meta": "relay:alice123",
            "actions": [
              { "label": "Message", "message": "Start chat with Alice", "payload": "{\"type\":\"start_conversation\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":true}]}}" },
              { "label": "Add contact", "message": "Add Alice", "payload": "{\"type\":\"add_contact\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":true}]}}" }
            ]
          }
        ]}
      ]
    })JSON";
  }

  if (lower.find("contacts") != std::string::npos || lower.find("conversations") != std::string::npos) {
    return R"JSON({
      "blocks": [
        { "type": "paragraph", "text": "Ask me to find someone on the network, or open a person thread from the sidebar." }
      ]
    })JSON";
  }

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
  DataModelHost::Instance().Dirty("chat", "has_turns");
}

void DirtyChatTurns() {
  DataModelHost::Instance().Dirty("chat", "turns");
  DataModelHost::Instance().Dirty("chat", "messages");
}

void DirtyChatHeader() {
  DataModelHost::Instance().Dirty("chat", "thread_title");
  DataModelHost::Instance().Dirty("chat", "thread_subtitle");
  DataModelHost::Instance().Dirty("chat", "draft_placeholder");
}

void DirtyChat() {
  DirtyChatChrome();
  DirtyChatTurns();
  DirtyChatHeader();
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

void ChatDemo::SelectThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSelectThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatDemo::OnSelectThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  if (auto thread = MessagingHub::Instance().Inbox().OpenThread(thread_id)) {
    RefreshFromMessaging();
    (void)thread;
  }
}

void ChatDemo::RefreshFromMessaging() {
  SyncShellSessions();
  SyncDisplayFromThread();
  UpdateThreadChrome();
  DirtyChat();
  DirtyShell();
}

void ChatDemo::SyncShellSessions() {
  if (!messaging_ready_) {
    return;
  }
  shell_.sessions.clear();
  auto threads = MessagingHub::Instance().Inbox().ListThreads();
  if (!threads) {
    return;
  }
  const std::string active_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  for (const Thread& thread : *threads) {
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = thread.title.c_str();
    row.preview = thread.preview.c_str();
    row.unread_count = thread.unread_count;
    row.active = thread.id == active_id;
    switch (thread.kind) {
    case ThreadKind::Ai:
      row.kind = "ai";
      break;
    case ThreadKind::Direct:
      row.kind = "direct";
      break;
    case ThreadKind::Group:
      row.kind = "group";
      break;
    }
    shell_.sessions.push_back(std::move(row));
  }
}

void ChatDemo::UpdateThreadChrome() {
  if (!messaging_ready_) {
    return;
  }
  if (auto thread = MessagingHub::Instance().Inbox().GetActiveThread()) {
    chat_.thread_title = thread->title.c_str();
    if (thread->kind == ThreadKind::Ai) {
      chat_.thread_subtitle = "AI home — ask to find people or open conversations";
      chat_.draft_placeholder = "Ask anything…";
    } else if (thread->kind == ThreadKind::Direct) {
      chat_.thread_subtitle = "Direct message";
      chat_.draft_placeholder = "Message… or @ai ask assistant";
    } else {
      chat_.thread_subtitle = "Group chat";
      chat_.draft_placeholder = "Message the group… or @ai ask assistant";
    }
  }
}

void ChatDemo::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  chat_.messages = MessagingHub::Instance().Inbox().BuildDisplayRows(thread_id);
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;
}

void ChatDemo::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    if (auto result = MessagingHub::Instance().Actions().Dispatch(*payload)) {
      if (*result) {
        SendUserText(message, *result);
        return;
      }
      RefreshFromMessaging();
      return;
    }
  }
  SendUserText(message, payload);
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
  auto perform_reset = [this]() {
    if (!messaging_ready_) {
      return;
    }
    (void)MessagingHub::Instance().Inbox().CreateAiHomeThread();
    chat_.draft = "";
    chat_.status = "";
    chat_.loading = false;
    pending_reply_.reset();
    ShellHost::Instance().SetAuxiliaryAvailable(false);
    ShellHost::Instance().CloseAuxiliary();
    ClearFormState();
    widgets_by_entry_.clear();
    RefreshFromMessaging();
  };

  ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "New chat", "Clear the current conversation?",
                             [perform_reset](bool ok) {
                               if (ok) {
                                 perform_reset();
                               }
                             });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
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

void ChatDemo::UpdateSidebarPreview(const std::string& preview_text) {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  (void)MessagingHub::Instance().Inbox().UpdatePreview(thread_id, preview_text);
  SyncShellSessions();
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

  const bool use_mock_reply = !use_llm_;
  bool expect_agent_work = use_mock_reply;
  if (!expect_agent_work && messaging_ready_ && MessagingHub::Instance().HasRouter()) {
    expect_agent_work = MessagingHub::Instance().Router().ExpectsAgentWork(
        MessagingHub::Instance().Inbox().ActiveThreadId(), trimmed, user_payload);
  } else if (!expect_agent_work) {
    expect_agent_work = true;
  }

  if (expect_agent_work) {
    chat_.loading = true;
    chat_.status = "";
  }
  UpdateSidebarPreview(trimmed);
  DirtyChatChrome();

  if (!use_llm_) {
    const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
    ThreadMessage user_message;
    user_message.id = GenerateUuid();
    user_message.thread_id = thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    user_message.text = trimmed;
    user_message.timestamp = NowUnixMs();
    (void)MessagingHub::Instance().Store().AppendMessage(user_message);
    SyncDisplayFromThread();
    DirtyChatTurns();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = user_message.id, .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  if (messaging_ready_ && MessagingHub::Instance().HasRouter()) {
    log().info << "Routing message via MessageRouter";
    (void)MessagingHub::Instance().Router().Route(MessagingHub::Instance().Inbox().ActiveThreadId(), trimmed,
                                                  std::move(user_payload));
    SyncDisplayFromThread();
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

  SyncDisplayFromThread();
  SendUserText(display_text, payload);
}

void ChatDemo::SendChatAction(const std::string& entry_id, int action_index) {
  if (chat_.loading || action_index < 0 || !messaging_ready_) {
    return;
  }

  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  auto messages = MessagingHub::Instance().Store().GetMessages(thread_id);
  if (!messages) {
    return;
  }

  for (const ThreadMessage& message : *messages) {
    if (message.id != entry_id) {
      continue;
    }
    if (action_index >= static_cast<int>(message.chat_actions.size())) {
      return;
    }
    const TranscriptChatAction& action = message.chat_actions[static_cast<size_t>(action_index)];
    HandleLocalAction(action.message, action.payload);
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
  SyncDisplayFromThread();
}

void ChatDemo::CalendarNext(const std::string& entry_id) {
  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }
  ShiftCalendarMonth(widgets->calendar, 1);
  SyncDisplayFromThread();
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
                                    const std::string& finish_reason, const std::string& thread_id) {
  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output)
                         : StructuredTextParser::ParseBlocksJson(raw_output);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      auto messages = MessagingHub::Instance().Store().GetMessages(active_thread);
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = "<div class=\"bubble bubble-assistant\" selectable=\"text\"><p class=\"error\">" +
                                  StructuredTextParser::EscapeText(parsed.error) + "</p></div>";
            (void)MessagingHub::Instance().Store().UpdateMessage(message);
            break;
          }
        }
      }
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

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, entry_id);
    hydrated = HydrateLegacyChatActions(hydrated, chat_actions);

    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      auto messages = MessagingHub::Instance().Store().GetMessages(active_thread);
      bool updated = false;
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = "<div class=\"bubble bubble-assistant\" selectable=\"text\">" + hydrated + "</div>";
            message.chat_actions = chat_actions;
            (void)MessagingHub::Instance().Store().UpdateMessage(message);
            updated = true;
            break;
          }
        }
      }
      if (!updated) {
        ThreadMessage ai_message;
        ai_message.id = GenerateUuid();
        ai_message.thread_id = active_thread;
        ai_message.sender_contact_id = kAiAssistantContactId;
        ai_message.text = raw_output;
        ai_message.content_rml =
            "<div class=\"bubble bubble-assistant\" selectable=\"text\">" + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.timestamp = NowUnixMs();
        (void)MessagingHub::Instance().Store().AppendMessage(ai_message);
      }
      (void)MessagingHub::Instance().Inbox().UpdatePreview(active_thread, parsed.rml);
    }

    shell_.preview_rml = Rml::String(hydrated.c_str());
    ShellHost::Instance().SetAuxiliaryAvailable(true);
    DirtyShell();
  }

  SyncDisplayFromThread();
  SyncShellSessions();
  chat_.loading = false;
  chat_.status = "";
  ShellHost::Instance().SetActivityVisible(false);
  DirtyChat();
  DirtyShell();
}

void ChatDemo::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      chat_.status = "";
      ShellHost::Instance().SetActivityVisible(false);
    } else {
      ShellHost::Instance().SetActivityVisible(true);
      if (messaging_ready_) {
        SyncDisplayFromThread();
        DirtyChatTurns();
      }
    }
    DirtyChatChrome();
    break;
  case AgentEventType::ToolActivity:
    chat_.status = Rml::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    ShellHost::Instance().SetActivityVisible(true);
    DirtyChatChrome();
    break;
  case AgentEventType::AssistantReady:
    FinishAssistantReply(event.entry_id, event.text, !StructuredTextParser::IsBlocksJsonDocument(event.text),
                       event.finish_reason, event.thread_id);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    ShellFeedback::ShowBanner(ShellHost::Instance().State(), event.message);
    ShellHost::Instance().DirtyWindow();
    if (agent_ && !agent_->conversation().Entries().empty()) {
      const TranscriptEntry& entry = agent_->conversation().Entries().back();
      agent_->CompleteAssistantMessage(entry.id, event.message);
      agent_->SetAssistantDisplay(entry.id, event.message, {});
      SyncDisplayFromThread();
      DirtyChatTurns();
    }
    chat_.loading = false;
    chat_.status = "";
    ShellHost::Instance().SetActivityVisible(false);
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

  if (auto init = MessagingHub::Instance().Initialize(config)) {
    messaging_ready_ = true;
    agent_->SetThreadStore(&MessagingHub::Instance().Store());
    MessagingHub::Instance().BindAgent(*agent_);
    MessagingHub::Instance().P2p().SetOnMessagesChanged([this]() { RefreshFromMessaging(); });
    MessagingHub::Instance().P2p().SetOnDeliveryNotice([this](const std::string& message) {
      ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
      ShellHost::Instance().DirtyWindow();
    });
    MessagingHub::Instance().Inbox().SetOnThreadChanged([this]() { RefreshFromMessaging(); });
    MessagingHub::Instance().Router().SetOnLocalAction(
        [this](const std::string& message, const std::optional<std::string>& payload) {
          HandleLocalAction(message, payload);
        });
    MessagingHub::Instance().Actions().SetOnActionMessage([this](const std::string& message) {
      ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
      ShellHost::Instance().DirtyWindow();
    });
    RefreshFromMessaging();
  }

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
        ctor.Bind("draft_placeholder", &ChatDemo::Instance().chat_.draft_placeholder);
        ctor.Bind("status", &ChatDemo::Instance().chat_.status);
        ctor.Bind("loading", &ChatDemo::Instance().chat_.loading);
        ctor.Bind("has_turns", &ChatDemo::Instance().chat_.has_turns);
        ctor.Bind("turns", &ChatDemo::Instance().chat_.turns);
        ctor.Bind("messages", &ChatDemo::Instance().chat_.messages);
        ctor.Bind("use_messages_layout", &ChatDemo::Instance().chat_.use_messages_layout);
        ctor.Bind("thread_title", &ChatDemo::Instance().chat_.thread_title);
        ctor.Bind("thread_subtitle", &ChatDemo::Instance().chat_.thread_subtitle);
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
          session_handle.RegisterMember("id", &ChatDemo::SessionRow::id);
          session_handle.RegisterMember("title", &ChatDemo::SessionRow::title);
          session_handle.RegisterMember("preview", &ChatDemo::SessionRow::preview);
          session_handle.RegisterMember("kind", &ChatDemo::SessionRow::kind);
          session_handle.RegisterMember("unread_count", &ChatDemo::SessionRow::unread_count);
          session_handle.RegisterMember("active", &ChatDemo::SessionRow::active);
        }
        ctor.RegisterArray<std::vector<ChatDemo::SessionRow>>();
        ctor.Bind("sessions", &ChatDemo::Instance().shell_.sessions);
        ctor.Bind("preview_rml", &ChatDemo::Instance().shell_.preview_rml);
        ctor.BindEventCallback("new_chat", &ChatDemo::NewChatCallback);
        ctor.BindEventCallback("select_thread", &ChatDemo::SelectThreadCallback);
        ctor.BindEventCallback("send_chat_action", &ChatDemo::SendChatActionCallback);
      })) {
    return false;
  }

  if (!ShellHost::RegisterWindowModel(context)) {
    return false;
  }

  InputCoordinator::Instance().Register(KeyBinding{
      .key = Rml::Input::KI_ESCAPE,
      .action = []() { return !ShellHost::Instance().HandleDismiss(); },
      .priority = 110,
  });

  ShellHost::Instance().Initialize(context);
  ShellHost::Instance().RegisterPane(
      {.key = "sidebar", .rml_path = "views/sidebar.rml", .role = PaneRole::Secondary, .toolbar_label = "Sessions"});
  ShellHost::Instance().RegisterPane({.key = "chat",
                                       .rml_path = "views/chat.rml",
                                       .role = PaneRole::Primary,
                                       .provides_composer = true});
  ShellHost::Instance().RegisterPane(
      {.key = "preview", .rml_path = "views/preview.rml", .role = PaneRole::Auxiliary, .toolbar_label = "Preview"});

  if (DocumentLoader::LoadFile(context, Application::AssetsPath("samples/window_shell.rml")) == nullptr) {
    return false;
  }

  ShellHost::Instance().Update(context);
  ShellHost::Instance().SyncLayout();

  if (!use_llm_) {
    ShellFeedback::ShowBanner(ShellHost::Instance().State(), "Using mock replies — LLM is not configured.");
    ShellHost::Instance().DirtyWindow();
  }

  return true;
}

void ChatDemo::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_) {
    MessagingHub::Instance().P2p().PollAndMerge();
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
  if (messaging_ready_) {
    MessagingHub::Instance().Shutdown();
    messaging_ready_ = false;
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
