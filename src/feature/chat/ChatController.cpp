#include "feature/chat/ChatController.h"
#include "base/platform/AppLifecycle.h"

#include "base/ai/StructuredTextParser.h"
#include "base/ai/WorkingSetPolicy.h"
#include "base/ai/conversation/Conversation.h"
#include "base/platform/IAssetLocator.h"
#include "base/ui/InputCoordinator.h"
#include "feature/chat/CalendarHelper.h"
#include "feature/chat/ChatFormHelper.h"
#include "feature/chat/ChatWidgetStateBuilder.h"
#include "common/Utilities.h"
#include "feature/messaging/MessagingHub.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/messaging/AtAiParser.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DocumentLoader.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/UserFeedback.h"
#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/ContactsController.h"

#include <nlohmann/json.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/SystemInterface.h>

#include <nlohmann/json.hpp>

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
  return Rml::String(("<div class=\"bubble bubble-user\" selectable=\"text\"><p class=\"bubble-text\">" + StructuredTextParser::EscapeText(text) +
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
          "Type a message and press Enter to send",
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
  DataModelHost::Instance().Dirty("chat", "thread_encrypted");
  DataModelHost::Instance().Dirty("chat", "thread_is_ai");
  DataModelHost::Instance().Dirty("chat", "thread_is_private");
  DataModelHost::Instance().Dirty("chat", "thread_is_public");
  DataModelHost::Instance().Dirty("chat", "thread_is_group");
  DataModelHost::Instance().Dirty("chat", "compose_disabled");
  DataModelHost::Instance().Dirty("chat", "draft_placeholder");
  DataModelHost::Instance().Dirty("chat", "show_thread_actions");
  DataModelHost::Instance().Dirty("chat", "show_forget_memory");
  DataModelHost::Instance().Dirty("chat", "show_sync_with_peer");
  DataModelHost::Instance().Dirty("chat", "show_thread_menu");
  DataModelHost::Instance().Dirty("chat", "show_gap_banner");
  DataModelHost::Instance().Dirty("chat", "show_compromised_banner");
  DataModelHost::Instance().Dirty("chat", "show_psk_setup_banner");
  DataModelHost::Instance().Dirty("chat", "show_psk_import");
  DataModelHost::Instance().Dirty("chat", "psk_has_key");
  DataModelHost::Instance().Dirty("chat", "psk_verified");
  DataModelHost::Instance().Dirty("chat", "psk_fingerprint");
  DataModelHost::Instance().Dirty("chat", "psk_export_b64");
  DataModelHost::Instance().Dirty("chat", "psk_import_text");
}

/** Sidebar / header visual type: ai | private | public | group */
Rml::String SessionVisualKind(const Thread& thread) {
  switch (thread.kind) {
  case ThreadKind::Ai:
    return "ai";
  case ThreadKind::Group:
    return "group";
  case ThreadKind::Direct:
    if (thread.channel == ThreadChannel::E2e) {
      return "private";
    }
    if (thread.channel == ThreadChannel::E2ePublic) {
      return "public";
    }
    return "public";
  }
  return "public";
}

void DirtyChat() {
  DirtyChatChrome();
  DirtyChatTurns();
  DirtyChatHeader();
}

void DirtyShell() {
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "working_set_active");
  DataModelHost::Instance().Dirty("shell", "working_set_title");
  DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
  DataModelHost::Instance().Dirty("shell", "working_set_rml");
  DataModelHost::Instance().Dirty("shell", "working_set");
}

} // namespace

ChatController::ChatController() {
  redirectLogger("ChatController");
}

ChatController& ChatController::Instance() {
  static ChatController controller;
  return controller;
}

void ChatController::OpenWorkingSetCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  const std::optional<int> block_index = EventArgAsInt(args, 1);
  if (!block_index || *block_index < 0) {
    return;
  }
  Instance().OpenWorkingSet(std::string(args[0].Get<Rml::String>().c_str()), *block_index);
}

void ChatController::DirtyWorkingSet() {
  DataModelHost::Instance().Dirty("shell", "working_set_active");
  DataModelHost::Instance().Dirty("shell", "working_set_title");
  DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
  DataModelHost::Instance().Dirty("shell", "working_set_rml");
  DataModelHost::Instance().Dirty("shell", "working_set");
}

std::vector<WorkingSetCandidate> ChatController::HydrateWorkingSetCandidates(
    const std::vector<WorkingSetCandidate>& candidates, const std::string& entry_id) const {
  std::vector<WorkingSetCandidate> hydrated;
  hydrated.reserve(candidates.size());
  for (WorkingSetCandidate candidate : candidates) {
    candidate.artifact_rml = InjectEntryPlaceholders(candidate.artifact_rml, entry_id);
    candidate.teaser_rml = InjectEntryPlaceholders(candidate.teaser_rml, entry_id);
    hydrated.push_back(std::move(candidate));
  }
  return hydrated;
}

void ChatController::SyncWorkingSetWidgetBindings(const std::string& entry_id) {
  shell_.working_set = {};
  if (const TurnWidgetState* widgets = FindWidgetState(entry_id)) {
    shell_.working_set = *widgets;
  }
  DirtyWorkingSet();
}

void ChatController::ClearWorkingSet() {
  shell_.working_set_active = false;
  shell_.working_set_title = "";
  shell_.working_set_subtitle = "";
  shell_.working_set_rml = "";
  shell_.working_set = {};
  active_working_set_affinity_ = WorkingSetAffinity::None;
  active_working_set_entry_id_.clear();
  ShellHost::Instance().SetAuxiliaryAvailable(false);
  ShellHost::Instance().CloseAuxiliary();
  DirtyWorkingSet();
}

void ChatController::OpenWorkingSet(const std::string& entry_id, const int block_index) {
  const auto entry_it = working_set_by_entry_.find(entry_id);
  if (entry_it == working_set_by_entry_.end()) {
    return;
  }

  const WorkingSetCandidate* selected = nullptr;
  for (const WorkingSetCandidate& candidate : entry_it->second) {
    if (candidate.block_index == block_index) {
      selected = &candidate;
      break;
    }
  }
  if (!selected) {
    return;
  }

  shell_.working_set_active = true;
  shell_.working_set_title = Rml::String(selected->title.c_str());
  shell_.working_set_subtitle = Rml::String(selected->subtitle.c_str());
  shell_.working_set_rml = Rml::String(selected->artifact_rml.c_str());
  active_working_set_affinity_ = selected->affinity;
  active_working_set_entry_id_ = entry_id;
  SyncWorkingSetWidgetBindings(entry_id);

  ShellHost::Instance().SetAuxiliaryAvailable(true);
  ShellHost::Instance().OpenAuxiliary();
  DirtyWorkingSet();
}

void ChatController::ApplyWorkingSetFromParse(const std::string& entry_id,
                                        const std::vector<WorkingSetCandidate>& candidates) {
  if (candidates.empty()) {
    ClearWorkingSet();
    return;
  }

  const std::vector<WorkingSetCandidate> hydrated = HydrateWorkingSetCandidates(candidates, entry_id);
  working_set_by_entry_[entry_id] = hydrated;

  const WorkingSetCandidate* primary = nullptr;
  for (const WorkingSetCandidate& candidate : hydrated) {
    if (candidate.auto_open) {
      primary = &candidate;
      break;
    }
  }
  if (!primary) {
    ClearWorkingSet();
    return;
  }

  const bool same_task = shell_.working_set_active && active_working_set_entry_id_ == entry_id &&
                         active_working_set_affinity_ == primary->affinity &&
                         active_working_set_affinity_ != WorkingSetAffinity::None;

  shell_.working_set_active = true;
  shell_.working_set_title = Rml::String(primary->title.c_str());
  shell_.working_set_subtitle = Rml::String(primary->subtitle.c_str());
  shell_.working_set_rml = Rml::String(primary->artifact_rml.c_str());
  active_working_set_affinity_ = primary->affinity;
  active_working_set_entry_id_ = entry_id;
  SyncWorkingSetWidgetBindings(entry_id);

  ShellHost::Instance().SetAuxiliaryAvailable(true);
  if (!same_task || !ShellHost::Instance().State().auxiliary_open) {
    ShellHost::Instance().OpenAuxiliary();
  }
  DirtyWorkingSet();
}

bool ChatController::ShouldCloseWorkingSetForAction(const std::optional<std::string>& payload) const {
  if (!payload || payload->empty()) {
    return false;
  }
  const nlohmann::json doc = nlohmann::json::parse(*payload, nullptr, false);
  if (doc.is_discarded() || !doc.is_object()) {
    return false;
  }
  const std::string type = doc.value("type", "");
  return type == "start_conversation" || type == "add_contact";
}

void ChatController::SendMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                   const Rml::VariantList& /*args*/) {
  Instance().OnSendMessage();
}

void ChatController::SendSuggestionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SendUserText(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::SubmitFormCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                  const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SubmitForm(std::string(args[0].Get<Rml::String>().c_str()),
                        std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatController::SendChatActionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
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

void ChatController::CalendarPrevCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarPrev(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::CalendarNextCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                    const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().CalendarNext(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::SelectCalendarDayCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                         const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SelectCalendarDay(std::string(args[0].Get<Rml::String>().c_str()),
                               std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatController::NewChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& /*args*/) {
  Instance().OnNewChat();
}

void ChatController::NewMessageCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& /*args*/) {
  Instance().OnNewMessage();
}

void ChatController::OpenNewSessionMenuCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnOpenNewSessionMenu(ev);
}

void ChatController::OpenThreadActionsMenuCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                   const Rml::VariantList& /*args*/) {
  Instance().OnOpenThreadActionsMenu(ev);
}

void ChatController::SelectThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSelectThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::CloseThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnCloseThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::ClearHistoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnClearHistory();
}

void ChatController::ForgetMemoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnForgetMemory();
}

void ChatController::SyncWithPeerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnSyncWithPeer();
}

void ChatController::RetryGapSyncCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnRetryGapSync();
}

void ChatController::StartNewSecureChatCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnStartNewSecureChat();
}

void ChatController::PauseIntegrityCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnPauseIntegrityOnly();
}

void ChatController::CopyPskKeyCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().OnCopyPskKey();
}

void ChatController::TogglePskImportCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnTogglePskImport();
}

void ChatController::ImportPskCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& /*args*/) {
  Instance().OnImportPsk();
}

void ChatController::VerifyPskCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                       const Rml::VariantList& /*args*/) {
  Instance().OnVerifyPsk();
}

void ChatController::RotatePskExportCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnRotatePskExport();
}

void ChatController::FinalizeThreadDisplay() {
  ClearWorkingSet();
  working_set_by_entry_.clear();
  RefreshFromMessaging();
  if (ShellHost::Instance().State().layout_mode == LayoutMode::Compact &&
      ShellHost::Instance().State().nav_tab == NavTab::Sessions) {
    ShellHost::Instance().OpenCompactChat();
  }
}

void ChatController::OnHomeTabActivated() {
  if (!messaging_ready_) {
    return;
  }
  (void)MessagingHub::Instance().Inbox().CreateAiHomeThread();
  ClearWorkingSet();
  working_set_by_entry_.clear();
  ShellHost::Instance().SetPrimaryPane("chat");
  RefreshFromMessaging();
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnSessionsTabActivated() {
  ClearWorkingSet();
  working_set_by_entry_.clear();
}

void ChatController::OnSelectThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  if (MessagingHub::Instance().Inbox().OpenThread(thread_id)) {
    MessagingHub::Instance().P2p().MaybeTailSync(thread_id);
    ShellHost::Instance().SetPrimaryPane("chat");
    FinalizeThreadDisplay();
  }
}

void ChatController::OnCloseThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }

  ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "Delete conversation",
                             "Delete this conversation? This cannot be undone.",
                             [this, thread_id](bool ok) {
                               if (!ok) {
                                 return;
                               }
                               if (!MessagingHub::Instance().Inbox().CloseThread(thread_id)) {
                                 return;
                               }
                               chat_.draft = "";
                               chat_.status = "";
                               chat_.loading = false;
                               pending_reply_.reset();
                               ClearFormState();
                               ClearWorkingSet();
                               working_set_by_entry_.clear();
                               widgets_by_entry_.clear();
                               chat_.turns.clear();
                               RefreshFromMessaging();
                               if (shell_.sessions.empty()) {
                                 ShellHost::Instance().SelectNavTab(NavTab::Home);
                                 ShellHost::Instance().CloseCompactChat();
                               }
                               ShellHost::Instance().RequestSyncLayout();
                               ShellHost::Instance().DirtyWindow();
                             });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnClearHistory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  auto thread = MessagingHub::Instance().Inbox().GetActiveThread();
  const bool is_ai = thread && thread->kind == ThreadKind::Ai;
  std::string message =
      "Remove all messages on this device? The thread stays in your sidebar. "
      "Messages on the peer's device or relay are not affected.";
  if (is_ai) {
    message += " AI memory is kept unless you check the box below.";
  } else if (thread && thread->kind == ThreadKind::Direct && thread->encrypted) {
    message += " Unsent outbound messages are cancelled; assigned sender_seq values are not reused.";
  }

  if (is_ai) {
    ShellFeedback::ShowConfirmWithCheckbox(
        ShellHost::Instance().State(), "Clear message history?", message, "Also forget what AI learned", false,
        [this, thread_id](bool ok, bool forget_memory) {
          if (!ok) {
            return;
          }
          if (!MessagingHub::Instance().Inbox().ClearThreadHistory(thread_id, forget_memory)) {
            return;
          }
          chat_.draft = "";
          chat_.status = "";
          chat_.loading = false;
          pending_reply_.reset();
          ClearFormState();
          widgets_by_entry_.clear();
          RefreshFromMessaging();
          ShellHost::Instance().RequestSyncLayout();
          ShellHost::Instance().DirtyWindow();
        });
  } else {
    ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "Clear message history?", message,
                               [this, thread_id](bool ok) {
                                 if (!ok) {
                                   return;
                                 }
                                 if (!MessagingHub::Instance().Inbox().ClearThreadHistory(thread_id, false)) {
                                   return;
                                 }
                                 chat_.draft = "";
                                 chat_.status = "";
                                 chat_.loading = false;
                                 pending_reply_.reset();
                                 ClearFormState();
                                 widgets_by_entry_.clear();
                                 RefreshFromMessaging();
                                 ShellHost::Instance().RequestSyncLayout();
                                 ShellHost::Instance().DirtyWindow();
                               });
  }
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnForgetMemory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirm(
      ShellHost::Instance().State(), "Forget what AI learned?",
      "Delete the durable conversation summary? Your message transcript stays on this device.",
      [this, thread_id](bool ok) {
        if (!ok) {
          return;
        }
        if (!MessagingHub::Instance().Inbox().ForgetThreadMemory(thread_id)) {
          return;
        }
        ShellHost::Instance().RequestSyncLayout();
        ShellHost::Instance().DirtyWindow();
      });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::LoadOlderHistoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnLoadOlderHistory();
}

void ChatController::OnLoadOlderHistory() {
  if (!messaging_ready_ || chat_.sync_in_progress) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  chat_.sync_in_progress = true;
  chat_.status = "Loading older messages…";
  DirtyChatChrome();

  MessagingHub::Instance().P2p().ScrollBackfill(thread_id, [this](Roe<ChatSyncResult> result) {
    chat_.sync_in_progress = false;
    chat_.status = "";
    if (!result) {
      chat_.status = result.error().message.c_str();
    } else if (result->ingested == 0) {
      chat_.show_older_history_hint = false;
    }
    RefreshFromMessaging();
    DirtyChatChrome();
  });
}

void ChatController::SendSharedAssistantRelay(const std::string& thread_id, const AtAiMode mode,
                                              const std::string& plain_text) {
  if (!messaging_ready_ || plain_text.empty()) {
    return;
  }
  auto thread = MessagingHub::Instance().Store().GetThread(thread_id);
  if (!thread || !*thread) {
    return;
  }

  SendRelayOptions opts;
  opts.sender_contact_id = kAiAssistantContactId;
  opts.generation = "ai_on_behalf";
  opts.ai_invoke_mode = mode == AtAiMode::SharedFull ? "shared_full" : "shared_reply";
  if (!(*thread)->participant_contact_ids.empty()) {
    opts.seq_owner_contact_id = (*thread)->participant_contact_ids.front();
  }
  opts.update_preview = true;
  (void)MessagingHub::Instance().P2p().SendUserMessage(thread_id, plain_text, opts);
}

void ChatController::OnSyncWithPeer() {
  if (!messaging_ready_ || chat_.sync_in_progress) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  chat_.sync_in_progress = true;
  chat_.status = "Syncing missing messages from peer…";
  DirtyChatChrome();

  MessagingHub::Instance().P2p().SyncWithPeer(thread_id, [this](Roe<ChatSyncResult> result) {
    chat_.sync_in_progress = false;
    if (result) {
      chat_.status = result->ingested > 0 ? "Sync complete." : "Up to date with peer.";
    } else {
      chat_.status = result.error().message.c_str();
    }
    RefreshFromMessaging();
    DirtyChatChrome();
    ShellHost::Instance().DirtyWindow();
  });
}

void ChatController::OnRetryGapSync() {
  if (!messaging_ready_ || chat_.sync_in_progress) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  chat_.sync_in_progress = true;
  chat_.status = "Retrying sync for missing messages…";
  DirtyChatChrome();

  MessagingHub::Instance().P2p().RetryGapSync(thread_id, [this](Roe<ChatSyncResult> result) {
    chat_.sync_in_progress = false;
    if (result) {
      if (result->ingested > 0 || result->empty_gap_closed) {
        chat_.status = "Gap repair complete.";
      } else {
        chat_.status = "No missing messages found for this gap.";
      }
    } else {
      chat_.status = result.error().message.c_str();
    }
    RefreshFromMessaging();
    DirtyChatChrome();
    ShellHost::Instance().DirtyWindow();
  });
}

void ChatController::OnStartNewSecureChat() {
  if (!messaging_ready_) {
    return;
  }
  WithSecrets([this]() {
    const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
    if (thread_id.empty()) {
      return;
    }

    ShellFeedback::ShowConfirm(
        ShellHost::Instance().State(), "Start new secure chat?",
        "This bumps the session epoch and cancels unsent messages from the previous epoch. "
        "Your saved transcript stays on this device.",
        [this, thread_id](bool ok) {
          if (!ok) {
            return;
          }
          auto result = MessagingHub::Instance().P2p().StartNewSecureChat(thread_id);
          if (!result) {
            chat_.status = result.error().message.c_str();
          } else {
            chat_.status = "New secure session started.";
          }
          RefreshFromMessaging();
          DirtyChatChrome();
          ShellHost::Instance().DirtyWindow();
        });
    ShellHost::Instance().RequestSyncLayout();
    ShellHost::Instance().DirtyWindow();
  });
}

void ChatController::OnPauseIntegrityOnly() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  if (!MessagingHub::Instance().P2p().PauseIntegrityOnly(thread_id)) {
    return;
  }
  chat_.status = "Messaging paused until you rotate the encryption key.";
  RefreshFromMessaging();
  DirtyChatHeader();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnCopyPskKey() {
  if (!messaging_ready_) {
    return;
  }
  WithSecrets([this]() {
    const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
    if (thread_id.empty()) {
      return;
    }
    auto exported = MessagingHub::Instance().P2p().EnsurePskGenerated(thread_id);
    if (!exported) {
      chat_.status = exported.error().message.c_str();
      DirtyChatChrome();
      return;
    }
    chat_.psk_export_b64 = exported->master_psk_b64.c_str();
    chat_.psk_fingerprint = exported->fingerprint.c_str();
    if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
      system->SetClipboardText(chat_.psk_export_b64);
    }
    chat_.status = "Encryption key copied.";
    DirtyChatHeader();
    ShellHost::Instance().DirtyWindow();
  });
}

void ChatController::OnTogglePskImport() {
  chat_.show_psk_import = !chat_.show_psk_import;
  DirtyChatHeader();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnImportPsk() {
  if (!messaging_ready_) {
    return;
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    WithSecrets([this]() { OnImportPsk(); });
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  const std::string pasted = chat_.psk_import_text.c_str();
  if (pasted.empty()) {
    chat_.status = "Paste a key or bundle first.";
    DirtyChatChrome();
    return;
  }

  if (pasted.find('{') != std::string::npos) {
    if (auto imported = MessagingHub::Instance().P2p().ImportPskBundleJson(thread_id, pasted); !imported) {
      chat_.status = imported.error().message.c_str();
    } else {
      chat_.psk_import_text = "";
      chat_.show_psk_import = false;
      chat_.status = "Encryption key installed. Verify the fingerprint before sending.";
    }
  } else if (auto imported = MessagingHub::Instance().P2p().ImportPskRawBase64(thread_id, pasted); !imported) {
    chat_.status = imported.error().message.c_str();
  } else {
    chat_.psk_import_text = "";
    chat_.show_psk_import = false;
    chat_.status = "Encryption key installed. Verify the fingerprint before sending.";
  }
  RefreshFromMessaging();
  DirtyChatHeader();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnVerifyPsk() {
  if (!messaging_ready_) {
    return;
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    WithSecrets([this]() { OnVerifyPsk(); });
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirmWithCheckbox(
      ShellHost::Instance().State(), "Verify encryption fingerprint",
      "Only confirm after you compared this fingerprint with your contact out of band.",
      "I've verified this fingerprint with my contact", false,
      [this, thread_id](const bool confirmed, const bool checked) {
        if (!confirmed || !checked) {
          return;
        }
        auto result = MessagingHub::Instance().P2p().MarkPskVerified(thread_id);
        if (!result) {
          chat_.status = result.error().message.c_str();
        } else {
          chat_.status = "Encryption key verified. You can send secure messages.";
        }
        RefreshFromMessaging();
        DirtyChatHeader();
        ShellHost::Instance().DirtyWindow();
      });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnRotatePskExport() {
  if (!messaging_ready_) {
    return;
  }
  if (!MessagingHub::Instance().IsMessagingReady()) {
    WithSecrets([this]() { OnRotatePskExport(); });
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShellFeedback::ShowConfirm(
      ShellHost::Instance().State(), "Rotate encryption key?",
      "This generates a new key, bumps the session epoch, and cancels unsent messages from the previous epoch. "
      "Share the exported bundle with your contact out of band.",
      [this, thread_id](const bool ok) {
        if (!ok) {
          return;
        }
        auto bundle = MessagingHub::Instance().P2p().RotatePskAndExportBundle(thread_id);
        if (!bundle) {
          chat_.status = bundle.error().message.c_str();
          DirtyChatChrome();
          ShellHost::Instance().DirtyWindow();
          return;
        }
        if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
          system->SetClipboardText(*bundle);
        }
        ShellFeedback::ShowAlert(
            ShellHost::Instance().State(), "Rotation bundle exported",
            "The pp-browser-psk-bundle-v1 JSON was copied to your clipboard. Send it to your contact securely.");
        chat_.status = "Encryption key rotated. Share the bundle with your contact.";
        RefreshFromMessaging();
        DirtyChatHeader();
        ShellHost::Instance().DirtyWindow();
      });
  ShellHost::Instance().RequestSyncLayout();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::RefreshFromMessaging() {
  SyncShellSessions();
  SyncDisplayFromThread();
  UpdateThreadChrome();
  DirtyChat();
  DirtyShell();
}

void ChatController::OnProfileDataReset() {
  messaging_ready_ = false;
  ClearWorkingSet();
  ClearFormState();
  widgets_by_entry_.clear();
  working_set_by_entry_.clear();
  pending_reply_.reset();
  ResetChatPanelState();
  if (MessagingHub::Instance().IsInitialized()) {
    WireMessagingBindings();
  } else {
    DirtyChat();
    DirtyShell();
  }
}

void ChatController::SyncShellSessions() {
  if (!messaging_ready_) {
    return;
  }
  shell_.sessions.clear();
  auto threads = MessagingHub::Instance().Inbox().ListThreads();
  if (!threads) {
    return;
  }
  std::vector<Thread> sorted_threads = *threads;
  std::sort(sorted_threads.begin(), sorted_threads.end(),
            [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });

  const std::string active_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  auto& inbox = MessagingHub::Instance().Inbox();
  for (const Thread& thread : sorted_threads) {
    if (inbox.IsAiHomeThread(thread.id)) {
      continue;
    }
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = thread.title.c_str();
    row.preview = thread.preview.c_str();
    row.kind = SessionVisualKind(thread);
    row.unread_count = thread.unread_count;
    row.active = thread.id == active_id;
    row.closable = !inbox.IsAiHomeThread(thread.id);
    shell_.sessions.push_back(std::move(row));
  }
}

void ChatController::ResetChatPanelState() {
  chat_.thread_title = "";
  chat_.thread_subtitle = "";
  chat_.thread_encrypted = false;
  chat_.thread_is_ai = false;
  chat_.thread_is_private = false;
  chat_.thread_is_public = false;
  chat_.thread_is_group = false;
  chat_.compose_disabled = false;
  chat_.show_thread_actions = false;
  chat_.show_forget_memory = false;
  chat_.show_sync_with_peer = false;
  chat_.show_thread_menu = false;
  chat_.show_gap_banner = false;
  chat_.show_older_history_hint = false;
  chat_.show_compromised_banner = false;
  chat_.show_psk_setup_banner = false;
  chat_.show_psk_import = false;
  chat_.psk_has_key = false;
  chat_.psk_verified = false;
  chat_.psk_fingerprint = "";
  chat_.psk_export_b64 = "";
  chat_.messages.clear();
  chat_.turns.clear();
  chat_.has_turns = false;
  chat_.use_messages_layout = true;
  chat_.draft_placeholder = "Ask anything…";
}

void ChatController::UpdateThreadChrome() {
  if (!messaging_ready_) {
    return;
  }
  if (auto thread = MessagingHub::Instance().Inbox().GetActiveThread()) {
    chat_.thread_title = thread->title.c_str();
    chat_.thread_encrypted = thread->encrypted;
    const Rml::String visual_kind = SessionVisualKind(*thread);
    chat_.thread_is_ai = visual_kind == "ai";
    chat_.thread_is_private = visual_kind == "private";
    chat_.thread_is_public = visual_kind == "public";
    chat_.thread_is_group = visual_kind == "group";
    chat_.show_thread_actions = true;
    chat_.show_forget_memory = thread->kind == ThreadKind::Ai;
    chat_.show_sync_with_peer = false;
    chat_.show_gap_banner = false;
    chat_.show_older_history_hint = false;
    chat_.show_compromised_banner = false;
    chat_.show_psk_setup_banner = false;
    chat_.show_psk_import = false;
    chat_.psk_has_key = false;
    chat_.psk_verified = false;
    chat_.psk_fingerprint = "";
    chat_.psk_export_b64 = "";
    if (thread->kind == ThreadKind::Direct && thread->channel == ThreadChannel::E2e) {
      if (auto epoch = MessagingHub::Instance().Store().GetChatTargetSessionEpoch(thread->id)) {
        if (auto sync_state = MessagingHub::Instance().Store().GetPeerSyncState(thread->id, *epoch)) {
          const bool compromised = sync_state->phase == PeerSyncPhase::Compromised;
          chat_.show_compromised_banner = compromised;
          chat_.compose_disabled = compromised;
          if (!compromised) {
            chat_.show_sync_with_peer = true;
            chat_.show_gap_banner = sync_state->phase == PeerSyncPhase::Gap;
            chat_.show_older_history_hint =
                sync_state->loaded_min_seq > sync_state->history_floor_seq + 1;
          }
        } else {
          chat_.show_sync_with_peer = true;
        }
      } else {
        chat_.show_sync_with_peer = true;
      }

      if (!chat_.show_compromised_banner) {
        if (!MessagingHub::Instance().IsMessagingReady()) {
          chat_.show_psk_setup_banner = true;
          chat_.compose_disabled = true;
        } else if (auto status = MessagingHub::Instance().P2p().GetPskStatus(thread->id)) {
          chat_.psk_has_key = status->has_psk;
          chat_.psk_verified = status->verified;
          chat_.psk_fingerprint = status->fingerprint.c_str();
          chat_.show_psk_setup_banner = !status->has_psk || !status->verified;
          if (status->has_psk) {
            if (auto exported = MessagingHub::Instance().P2p().GetPskExportView(thread->id)) {
              chat_.psk_export_b64 = exported->master_psk_b64.c_str();
            }
          } else if (auto generated = MessagingHub::Instance().P2p().EnsurePskGenerated(thread->id)) {
            chat_.psk_has_key = true;
            chat_.psk_fingerprint = generated->fingerprint.c_str();
            chat_.psk_export_b64 = generated->master_psk_b64.c_str();
            chat_.show_psk_setup_banner = true;
          }
          chat_.compose_disabled = !status->has_psk || !status->verified;
        }
      }
    }
    if (thread->kind == ThreadKind::Ai) {
      chat_.thread_subtitle = "Local assistant";
      chat_.draft_placeholder = "Ask anything…";
    } else if (thread->kind == ThreadKind::Direct) {
      if (thread->channel == ThreadChannel::E2ePublic) {
        chat_.thread_subtitle = "Encrypted · easy start";
        chat_.draft_placeholder = "Message… · @ai · @ai+ · @ai++";
      } else if (thread->channel == ThreadChannel::E2e) {
        chat_.thread_subtitle = "Verified private · E2E";
        chat_.draft_placeholder = "Secure message… · @ai · @ai+ · @ai++";
      } else {
        chat_.thread_subtitle = "Direct message";
        chat_.draft_placeholder = "Message… · @ai · @ai+ · @ai++";
      }
    } else {
      chat_.thread_subtitle = thread->encrypted ? "Group · E2E" : "Group chat";
      chat_.draft_placeholder = "Message the group… or @ai ask assistant";
    }
    chat_.show_thread_menu =
        chat_.show_thread_actions || chat_.show_forget_memory || chat_.show_sync_with_peer;
  } else {
    chat_.thread_title = "";
    chat_.thread_subtitle = "";
    chat_.thread_encrypted = false;
    chat_.thread_is_ai = false;
    chat_.thread_is_private = false;
    chat_.thread_is_public = false;
    chat_.thread_is_group = false;
    chat_.compose_disabled = false;
    chat_.show_thread_actions = false;
    chat_.show_forget_memory = false;
    chat_.show_sync_with_peer = false;
    chat_.show_thread_menu = false;
    chat_.show_gap_banner = false;
    chat_.show_older_history_hint = false;
    chat_.show_compromised_banner = false;
    chat_.show_psk_setup_banner = false;
    chat_.show_psk_import = false;
    chat_.psk_has_key = false;
    chat_.psk_verified = false;
    chat_.psk_fingerprint = "";
    chat_.psk_export_b64 = "";
    chat_.draft_placeholder = "Ask anything…";
  }
}

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  auto& inbox = MessagingHub::Instance().Inbox();
  if (!inbox.GetActiveThread()) {
    ResetChatPanelState();
    return;
  }
  const std::string thread_id = inbox.ActiveThreadId();
  chat_.messages = inbox.BuildDisplayRows(thread_id);
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;
}

void ChatController::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    if (auto result = MessagingHub::Instance().Actions().Dispatch(*payload)) {
      if (*result) {
        SendUserText(message, *result);
        return;
      }
      RefreshFromMessaging();
      ContactsController::Instance().Refresh();
      const std::string active_id = MessagingHub::Instance().Inbox().ActiveThreadId();
      auto& inbox = MessagingHub::Instance().Inbox();
      if (inbox.IsAiHomeThread(active_id)) {
        ShellHost::Instance().SelectNavTab(NavTab::Home);
      } else {
        ShellHost::Instance().SelectNavTab(NavTab::Sessions);
      }
      ShellHost::Instance().SetPrimaryPane("chat");
      if (ShellHost::Instance().State().layout_mode == LayoutMode::Compact &&
          ShellHost::Instance().State().nav_tab == NavTab::Sessions) {
        ShellHost::Instance().OpenCompactChat();
      }
      return;
    }
  }
  SendUserText(message, payload);
}

void ChatController::OnSendMessage() {
  if (chat_.loading || chat_.compose_disabled) {
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

void ChatController::OnNewChat() {
  if (!messaging_ready_) {
    return;
  }

  (void)MessagingHub::Instance().Inbox().CreateNewAiThread();
  chat_.draft = "";
  chat_.status = "";
  chat_.loading = false;
  chat_.compose_disabled = false;
  pending_reply_.reset();
  if (agent_) {
    agent_->Cancel();
  }
  ClearWorkingSet();
  working_set_by_entry_.clear();
  ClearFormState();
  widgets_by_entry_.clear();
  ShellHost::Instance().SetPrimaryPane("chat");
  focus_draft_after_sync_ = true;
  FinalizeThreadDisplay();
  ShellHost::Instance().DirtyWindow();
}

void ChatController::OnNewMessage() {
  ShellHost::Instance().SelectNavTab(NavTab::Contacts);
}

void ChatController::OnOpenNewSessionMenu(Rml::Event& ev) {
  Rml::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  Rml::Vector2i position{0, 0};
  if (target) {
    const Rml::Vector2f offset = target->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Box& box = target->GetBox();
    position.x = static_cast<int>(offset.x);
    position.y = static_cast<int>(offset.y + box.GetSize(Rml::BoxArea::Border).y + 4.0f);
  }

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      "Chat with AI",
      nullptr,
      []() { ChatController::Instance().OnNewChat(); },
      "../icons/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      "Message a contact",
      nullptr,
      []() { ChatController::Instance().OnNewMessage(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      "Find someone",
      nullptr,
      []() { ChatController::Instance().OnFindSomeone(); },
      "../icons/message.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenThreadActionsMenu(Rml::Event& ev) {
  Rml::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  Rml::Vector2i position{0, 0};
  if (target) {
    const Rml::Vector2f offset = target->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Box& box = target->GetBox();
    const Rml::Vector2f size = box.GetSize(Rml::BoxArea::Border);
    // Anchor near the right edge of the trigger so the menu stays in the header corner.
    position.x = static_cast<int>(offset.x + size.x - 180.0f);
    if (position.x < 0) {
      position.x = static_cast<int>(offset.x);
    }
    position.y = static_cast<int>(offset.y + size.y + 4.0f);
  }

  std::vector<ContextMenuAction> actions;
  if (chat_.show_sync_with_peer && !chat_.sync_in_progress) {
    actions.push_back({
        "sync_with_peer",
        "Sync with peer",
        nullptr,
        []() { ChatController::Instance().OnSyncWithPeer(); },
        "../icons/sync.svg",
    });
  }
  if (chat_.show_thread_actions) {
    actions.push_back({
        "clear_history",
        "Clear history…",
        nullptr,
        []() { ChatController::Instance().OnClearHistory(); },
        "../icons/trash.svg",
        true,
    });
  }
  if (chat_.show_forget_memory) {
    actions.push_back({
        "forget_memory",
        "Forget AI memory…",
        nullptr,
        []() { ChatController::Instance().OnForgetMemory(); },
        "../icons/sparkle.svg",
        true,
    });
  }
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnFindSomeone() {
  if (!messaging_ready_) {
    return;
  }
  OnNewChat();
  focus_draft_after_sync_ = true;
  chat_.draft = "Find someone on the network";
  DirtyChatChrome();
}

void ChatController::OnShellLayoutSynced() {
  if (!focus_draft_after_sync_) {
    return;
  }
  focus_draft_after_sync_ = false;
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  if (Rml::Element* draft = context_->GetDocument(0)->GetElementById("draft-input")) {
    draft->Focus();
  }
}

bool ChatController::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  if (submitted_forms_.count({entry_id, form_id}) > 0) {
    return false;
  }
  return active_form_ && active_form_->entry_id == entry_id && active_form_->form_id == form_id;
}

TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) {
  const auto it = widgets_by_entry_.find(entry_id);
  return it == widgets_by_entry_.end() ? nullptr : &it->second;
}

const TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) const {
  const auto it = widgets_by_entry_.find(entry_id);
  return it == widgets_by_entry_.end() ? nullptr : &it->second;
}

void ChatController::MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
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

std::string ChatController::HydrateAssistantRml(const TranscriptEntry& entry) const {
  if (!entry.assistant_rml) {
    return {};
  }

  std::string rml = HydrateLegacyChatActions(*entry.assistant_rml, entry.chat_actions);
  return InjectEntryPlaceholders(rml, entry.id);
}

void ChatController::InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
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

void ChatController::ExpireFormsExcept(const std::string& entry_id, const std::string& form_id) {
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

void ChatController::ClearFormState() {
  active_form_.reset();
  submitted_forms_.clear();
}

void ChatController::UpdateSidebarPreview(const std::string& preview_text) {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  (void)MessagingHub::Instance().Inbox().UpdatePreview(thread_id, preview_text);
  SyncShellSessions();
  DirtyShell();
}

void ChatController::SendUserText(const std::string& text, std::optional<std::string> user_payload) {
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
    user_message.id = util::GenerateUuid();
    user_message.thread_id = thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    user_message.text = trimmed;
    user_message.timestamp = util::NowUnixMs();
    user_message.transport = MessageTransport::Local;
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

void ChatController::SubmitForm(const std::string& entry_id, const std::string& form_id) {
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
  ClearWorkingSet();

  SyncDisplayFromThread();
  SendUserText(display_text, payload);
}

void ChatController::SendChatAction(const std::string& entry_id, int action_index) {
  if (chat_.loading || action_index < 0 || !messaging_ready_) {
    return;
  }

  const std::string thread_id = MessagingHub::Instance().Inbox().ActiveThreadId();
  auto messages = MessagingHub::Instance().Store().GetMessagesPage(thread_id, std::nullopt, 10000);
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
    if (ShouldCloseWorkingSetForAction(action.payload)) {
      ClearWorkingSet();
    }
    HandleLocalAction(action.message, action.payload);
    return;
  }

  log().warning << "Chat action entry not found: " << entry_id;
}

void ChatController::CalendarPrev(const std::string& entry_id) {
  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }
  ShiftCalendarMonth(widgets->calendar, -1);
  if (active_working_set_entry_id_ == entry_id) {
    SyncWorkingSetWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::CalendarNext(const std::string& entry_id) {
  TurnWidgetState* widgets = FindWidgetState(entry_id);
  if (!widgets || !widgets->has_calendar) {
    return;
  }
  ShiftCalendarMonth(widgets->calendar, 1);
  if (active_working_set_entry_id_ == entry_id) {
    SyncWorkingSetWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::SelectCalendarDay(const std::string& entry_id, const std::string& iso_date) {
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

void ChatController::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, const bool from_llm,
                                    const std::string& finish_reason, const std::string& thread_id,
                                    ResponseGoal response_goal, RenderMode render_mode, const AtAiMode shared_ai_mode) {
  if (!from_llm) {
    response_goal = InferResponseGoalFromBlocksJson(raw_output);
  }

  auto parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output, response_goal, render_mode)
                         : StructuredTextParser::ParseBlocksJson(raw_output, response_goal, render_mode);
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      auto messages = MessagingHub::Instance().Store().GetMessagesPage(active_thread, std::nullopt, 10000);
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
      auto messages = MessagingHub::Instance().Store().GetMessagesPage(active_thread, std::nullopt, 10000);
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
        ai_message.id = util::GenerateUuid();
        ai_message.thread_id = active_thread;
        ai_message.sender_contact_id = kAiAssistantContactId;
        ai_message.text = raw_output;
        ai_message.content_rml =
            "<div class=\"bubble bubble-assistant\" selectable=\"text\">" + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.timestamp = util::NowUnixMs();
        ai_message.transport = MessageTransport::Local;
        (void)MessagingHub::Instance().Store().AppendMessage(ai_message);
      }
      (void)MessagingHub::Instance().Inbox().UpdatePreview(active_thread, parsed.rml);
    }

    ApplyWorkingSetFromParse(entry_id, parsed.working_set_candidates);

    if (shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull) {
      const std::string active_thread =
          thread_id.empty() ? MessagingHub::Instance().Inbox().ActiveThreadId() : thread_id;
      std::string relay_plain = raw_output;
      if (StructuredTextParser::IsBlocksJsonDocument(raw_output)) {
        try {
          const nlohmann::json blocks_doc = nlohmann::json::parse(raw_output);
          if (blocks_doc.contains("blocks") && blocks_doc["blocks"].is_array()) {
            std::string joined;
            for (const auto& block : blocks_doc["blocks"]) {
              if (block.value("type", "") == "paragraph" && block.contains("text") &&
                  block["text"].is_string()) {
                if (!joined.empty()) {
                  joined += "\n";
                }
                joined += block["text"].get<std::string>();
              }
            }
            if (!joined.empty()) {
              relay_plain = joined;
            }
          }
        } catch (const std::exception&) {
        }
      }
      SendSharedAssistantRelay(active_thread, shared_ai_mode, relay_plain);
    }
  }

  SyncDisplayFromThread();
  SyncShellSessions();
  chat_.loading = false;
  chat_.status = "";
  ShellHost::Instance().SetActivityVisible(false);
  DirtyChat();
  DirtyShell();
}

void ChatController::HandleAgentEvent(const AgentEvent& event) {
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
                         event.finish_reason, event.thread_id, event.response_goal, event.render_mode,
                         event.shared_ai_mode);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    UserFeedback::NeedsSetup(event.message);
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

void ChatController::WithSecrets(std::function<void()> action) {
  PinGateController::Instance().EnsureUnlocked(
      [this, action = std::move(action)](const bool unlocked) {
        if (!unlocked) {
          ShellFeedback::ShowToast(ShellHost::Instance().State(), "PIN required to continue");
          ShellHost::Instance().DirtyWindow();
          return;
        }
        if (action) {
          action();
        }
      });
}

void ChatController::WireMessagingBindings() {
  if (!MessagingHub::Instance().IsInitialized() || !agent_) {
    return;
  }
  messaging_ready_ = true;
  agent_->SetThreadStore(&MessagingHub::Instance().Store());
  MessagingHub::Instance().BindAgent(*agent_);
  MessagingHub::Instance().P2p().SetOnMessagesChanged([this]() { RefreshFromMessaging(); });
  MessagingHub::Instance().P2p().SetOnDeliveryNotice([this](const std::string& message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
    ShellHost::Instance().DirtyWindow();
  });
  MessagingHub::Instance().Inbox().SetOnThreadChanged([this]() { RefreshFromMessaging(); });
  if (MessagingHub::Instance().HasRouter()) {
    MessagingHub::Instance().Router().SetOnLocalAction(
        [this](const std::string& message, const std::optional<std::string>& payload) {
          HandleLocalAction(message, payload);
        });
    MessagingHub::Instance().Router().SetSharedAiConfirmCallback(
        [this](const std::string& thread_id, const AtAiMode mode, const std::string& prompt,
               std::function<void(bool confirmed, bool dont_ask_again)> done) {
          const char* mode_label = mode == AtAiMode::SharedFull ? "share the prompt and reply" : "share the AI reply";
          ShellFeedback::ShowConfirmWithCheckbox(
              ShellHost::Instance().State(), "Share with peer?",
              std::string("This will ") + mode_label +
                  " on the encrypted thread. Your local @ai assist stays private.",
              "Don't ask again for this conversation", false,
              [this, thread_id, done = std::move(done)](const bool ok, const bool dont_ask) {
                if (ok && dont_ask) {
                  MessagingHub::Instance().Router().MarkSharedAiConfirmed(thread_id);
                }
                done(ok, dont_ask);
              });
        });
  }
  MessagingHub::Instance().Actions().SetOnActionMessage([this](const std::string& message) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), message);
    ShellHost::Instance().DirtyWindow();
  });
  RefreshFromMessaging();
  if (MessagingHub::Instance().IsMessagingReady()) {
    MessagingHub::Instance().P2p().TailSyncActiveE2eThread();
  }
}

bool ChatController::Setup(Rml::Context* context) {
  if (!context) {
    return false;
  }

  context_ = context;
  AppLifecycle::AddBackgroundListener([this]() { OnApplicationPause(); });
  AppLifecycle::AddForegroundListener([this]() {
    if (!messaging_ready_) {
      return;
    }
    const std::string active = MessagingHub::Instance().Inbox().ActiveThreadId();
    if (!active.empty() && MessagingHub::Instance().IsMessagingReady()) {
      MessagingHub::Instance().P2p().WarmPeerForThread(active);
    }
  });
  const AppConfig& config = SessionStore::Instance().Snapshot().config;
  ClearFormState();
  widgets_by_entry_.clear();
  chat_ = {};
  shell_ = {};
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  agent_.emplace();

  if (MessagingHub::Instance().IsInitialized()) {
    WireMessagingBindings();
    MessagingHub::Instance().SetOnMessagingReady([this]() {
      WireMessagingBindings();
      if (ShellHost::Instance().State().nav_tab == NavTab::Me) {
        SettingsController::Instance().OnNavTabActivated();
      }
    });
  }

  agent_->Configure(config);
  log().info << "Chat initialized (model: " << config.llm.model << ")";

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
        ctor.Bind("draft", &ChatController::Instance().chat_.draft);
        ctor.Bind("draft_placeholder", &ChatController::Instance().chat_.draft_placeholder);
        ctor.Bind("status", &ChatController::Instance().chat_.status);
        ctor.Bind("loading", &ChatController::Instance().chat_.loading);
        ctor.Bind("has_turns", &ChatController::Instance().chat_.has_turns);
        ctor.Bind("turns", &ChatController::Instance().chat_.turns);
        ctor.Bind("messages", &ChatController::Instance().chat_.messages);
        ctor.Bind("use_messages_layout", &ChatController::Instance().chat_.use_messages_layout);
        ctor.Bind("thread_title", &ChatController::Instance().chat_.thread_title);
        ctor.Bind("thread_subtitle", &ChatController::Instance().chat_.thread_subtitle);
        ctor.Bind("thread_encrypted", &ChatController::Instance().chat_.thread_encrypted);
        ctor.Bind("thread_is_ai", &ChatController::Instance().chat_.thread_is_ai);
        ctor.Bind("thread_is_private", &ChatController::Instance().chat_.thread_is_private);
        ctor.Bind("thread_is_public", &ChatController::Instance().chat_.thread_is_public);
        ctor.Bind("thread_is_group", &ChatController::Instance().chat_.thread_is_group);
        ctor.Bind("compose_disabled", &ChatController::Instance().chat_.compose_disabled);
        ctor.Bind("show_thread_actions", &ChatController::Instance().chat_.show_thread_actions);
        ctor.Bind("show_forget_memory", &ChatController::Instance().chat_.show_forget_memory);
        ctor.Bind("show_sync_with_peer", &ChatController::Instance().chat_.show_sync_with_peer);
        ctor.Bind("show_thread_menu", &ChatController::Instance().chat_.show_thread_menu);
        ctor.Bind("show_gap_banner", &ChatController::Instance().chat_.show_gap_banner);
        ctor.Bind("show_compromised_banner", &ChatController::Instance().chat_.show_compromised_banner);
        ctor.Bind("show_psk_setup_banner", &ChatController::Instance().chat_.show_psk_setup_banner);
        ctor.Bind("show_psk_import", &ChatController::Instance().chat_.show_psk_import);
        ctor.Bind("psk_has_key", &ChatController::Instance().chat_.psk_has_key);
        ctor.Bind("psk_verified", &ChatController::Instance().chat_.psk_verified);
        ctor.Bind("psk_fingerprint", &ChatController::Instance().chat_.psk_fingerprint);
        ctor.Bind("psk_export_b64", &ChatController::Instance().chat_.psk_export_b64);
        ctor.Bind("psk_import_text", &ChatController::Instance().chat_.psk_import_text);
        ctor.Bind("sync_in_progress", &ChatController::Instance().chat_.sync_in_progress);
        ctor.Bind("show_older_history_hint", &ChatController::Instance().chat_.show_older_history_hint);
        ctor.BindEventCallback("send_message", &ChatController::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatController::SendSuggestionCallback);
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("submit_form", &ChatController::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatController::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatController::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatController::SelectCalendarDayCallback);
        ctor.BindEventCallback("open_working_set", &ChatController::OpenWorkingSetCallback);
        ctor.BindEventCallback("clear_history", &ChatController::ClearHistoryCallback);
        ctor.BindEventCallback("forget_memory", &ChatController::ForgetMemoryCallback);
        ctor.BindEventCallback("open_thread_actions_menu", &ChatController::OpenThreadActionsMenuCallback);
        ctor.BindEventCallback("sync_with_peer", &ChatController::SyncWithPeerCallback);
        ctor.BindEventCallback("retry_gap_sync", &ChatController::RetryGapSyncCallback);
        ctor.BindEventCallback("start_new_secure_chat", &ChatController::StartNewSecureChatCallback);
        ctor.BindEventCallback("pause_integrity_only", &ChatController::PauseIntegrityCallback);
        ctor.BindEventCallback("copy_psk_key", &ChatController::CopyPskKeyCallback);
        ctor.BindEventCallback("toggle_psk_import", &ChatController::TogglePskImportCallback);
        ctor.BindEventCallback("import_psk", &ChatController::ImportPskCallback);
        ctor.BindEventCallback("verify_psk", &ChatController::VerifyPskCallback);
        ctor.BindEventCallback("rotate_psk_export", &ChatController::RotatePskExportCallback);
        ctor.BindEventCallback("load_older_history", &ChatController::LoadOlderHistoryCallback);
        ctor.BindEventCallback("new_message", &ChatController::NewMessageCallback);
      })) {
    return false;
  }

  if (!DataModelHost::Instance().Register(context, "shell", [](Rml::DataModelConstructor& ctor) {
        RegisterChatWidgetDataTypes(ctor);
        if (auto working_set_handle = ctor.RegisterStruct<TurnWidgetState>()) {
          working_set_handle.RegisterMember("has_form", &TurnWidgetState::has_form);
          working_set_handle.RegisterMember("form", &TurnWidgetState::form);
          working_set_handle.RegisterMember("has_calendar", &TurnWidgetState::has_calendar);
          working_set_handle.RegisterMember("calendar", &TurnWidgetState::calendar);
        }
        if (auto session_handle = ctor.RegisterStruct<ChatController::SessionRow>()) {
          session_handle.RegisterMember("id", &ChatController::SessionRow::id);
          session_handle.RegisterMember("title", &ChatController::SessionRow::title);
          session_handle.RegisterMember("preview", &ChatController::SessionRow::preview);
          session_handle.RegisterMember("kind", &ChatController::SessionRow::kind);
          session_handle.RegisterMember("unread_count", &ChatController::SessionRow::unread_count);
          session_handle.RegisterMember("active", &ChatController::SessionRow::active);
          session_handle.RegisterMember("closable", &ChatController::SessionRow::closable);
        }
        ctor.RegisterArray<std::vector<ChatController::SessionRow>>();
        ctor.Bind("sessions", &ChatController::Instance().shell_.sessions);
        ctor.Bind("working_set_active", &ChatController::Instance().shell_.working_set_active);
        ctor.Bind("working_set_title", &ChatController::Instance().shell_.working_set_title);
        ctor.Bind("working_set_subtitle", &ChatController::Instance().shell_.working_set_subtitle);
        ctor.Bind("working_set_rml", &ChatController::Instance().shell_.working_set_rml);
        ctor.Bind("working_set", &ChatController::Instance().shell_.working_set);
        ctor.BindEventCallback("new_chat", &ChatController::NewChatCallback);
        ctor.BindEventCallback("new_message", &ChatController::NewMessageCallback);
        ctor.BindEventCallback("open_new_session_menu", &ChatController::OpenNewSessionMenuCallback);
        ctor.BindEventCallback("select_thread", &ChatController::SelectThreadCallback);
        ctor.BindEventCallback("close_thread", &ChatController::CloseThreadCallback);
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("submit_form", &ChatController::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatController::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatController::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatController::SelectCalendarDayCallback);
        ctor.BindEventCallback("open_working_set", &ChatController::OpenWorkingSetCallback);
      })) {
    return false;
  }

  if (!ShellHost::RegisterWindowModel(context)) {
    return false;
  }

  if (!SettingsController::Instance().RegisterModel(context)) {
    return false;
  }

  if (!ContactsController::Instance().RegisterModel(context)) {
    return false;
  }

  SessionStore::Instance().AddConfigListener([this](const AppConfig& updated) { ApplyRuntimeConfig(updated); });

  ShellHost::Instance().Initialize(context);
  ShellHost::Instance().SetOnNavTabChanged([](NavTab tab) {
    static NavTab previous_tab = NavTab::Home;
    if (previous_tab == NavTab::Me && tab != NavTab::Me) {
      SettingsController::Instance().OnNavTabDeactivated();
    }
    if (tab == NavTab::Home) {
      ChatController::Instance().OnHomeTabActivated();
    }
    if (tab == NavTab::Sessions) {
      ChatController::Instance().OnSessionsTabActivated();
    }
    if (tab == NavTab::Me) {
      SettingsController::Instance().OnNavTabActivated();
    }
    if (tab == NavTab::Contacts) {
      ContactsController::Instance().OnNavTabActivated();
    }
    previous_tab = tab;
  });
  ShellHost::Instance().SetOnLayoutModeChanged([](LayoutMode mode) {
    if (ShellHost::Instance().State().nav_tab == NavTab::Contacts) {
      ContactsController::Instance().SyncLayoutMode();
    }
    if (ShellHost::Instance().State().nav_tab == NavTab::Me) {
      SettingsController::Instance().SyncLayoutMode();
    }
    if (mode == LayoutMode::Compact && ShellHost::Instance().State().nav_tab == NavTab::Home) {
      ChatController::Instance().OnHomeTabActivated();
    }
  });
  ShellHost::Instance().SetOnLayoutSynced([]() {
    SettingsController::Instance().OnShellLayoutSynced();
    ChatController::Instance().OnShellLayoutSynced();
  });
  ShellHost::Instance().RegisterPane(
      {.key = "sidebar", .rml_path = "views/sidebar.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane(
      {.key = "contacts", .rml_path = "views/contacts.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane(
      {.key = "settings", .rml_path = "views/settings.rml", .role = PaneRole::Secondary});
  ShellHost::Instance().RegisterPane({.key = "chat",
                                       .rml_path = "views/chat.rml",
                                       .role = PaneRole::Primary,
                                       .provides_composer = true});
  ShellHost::Instance().RegisterPane(
      {.key = "contact_detail", .rml_path = "views/contact_detail.rml", .role = PaneRole::Primary});
  ShellHost::Instance().RegisterPane(
      {.key = "settings_detail", .rml_path = "views/settings_detail.rml", .role = PaneRole::Primary});
  ShellHost::Instance().RegisterPane(
      {.key = "preview", .rml_path = "views/preview.rml", .role = PaneRole::Auxiliary, .toolbar_label = "Preview"});

  if (DocumentLoader::LoadFile(context, IAssetLocator::Instance().Resolve("samples/window_shell.rml")) == nullptr) {
    return false;
  }

  ShellHost::Instance().Update(context);
  ShellHost::Instance().SyncLayout();

  PinGateController::Instance().PromptUnlockIfVaultExists();

  if (messaging_ready_) {
    OnHomeTabActivated();
  }

  if (!use_llm_) {
    UserFeedback::NeedsSetup("Using mock replies — LLM is not configured.");
  } else if (ResolvePreset(config) == "brief") {
    std::string brief_key;
    if (MessagingHub::Instance().IsInitialized()) {
      if (auto identity = MessagingHub::Instance().Identity().Get()) {
        brief_key = identity->brief_llm_api_key;
      }
    }
    if (brief_key.empty()) {
      UserFeedback::NeedsSetup(
          "Register your identity in Me → Profile to use Brief assistant (or switch to Cloud/Ollama).");
    }
  } else if (config.llm.require_api_key && config.llm.api_key.empty()) {
    UserFeedback::NeedsSetup("Add your API key in Me → Assistant to enable the assistant.");
  }

  return true;
}

void ChatController::ApplyRuntimeConfig(const AppConfig& config) {
  use_llm_ = !config.llm.base_url.empty();
  if (agent_) {
    agent_->Configure(config);
  }
}

void ChatController::ReloadAgentConfig() {
  ApplyRuntimeConfig(SessionStore::Instance().Snapshot().config);
}

void ChatController::OnApplicationPause() {
  if (agent_) {
    agent_->Cancel();
  }
  if (messaging_ready_) {
    MessagingHub::Instance().SuspendLibp2pColdPeers();
  }
}

void ChatController::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_ && AppLifecycle::IsForeground()) {
    MessagingHub::Instance().TickLibp2p();
    if (MessagingHub::Instance().IsMessagingReady()) {
      MessagingHub::Instance().P2p().PollAndMerge();
    }
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

void ChatController::Shutdown() {
  AppLifecycle::ClearBackgroundListeners();
  AppLifecycle::ClearForegroundListeners();
  if (agent_) {
    agent_->Cancel();
    agent_.reset();
  }
  if (messaging_ready_) {
    MessagingHub::Instance().Shutdown();
    ProfileSecretsService::Instance().Shutdown();
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

bool SetupChatController(Rml::Context* context) {
  return ChatController::Instance().Setup(context);
}

void UpdateChatController() {
  if (ShellHost::Instance().State().nav_tab == NavTab::Me) {
    SettingsController::Instance().Tick();
  }
  if (ShellHost::Instance().State().nav_tab == NavTab::Contacts) {
    ContactsController::Instance().Tick();
  }
  ChatController::Instance().Update();
}

void ShutdownChatController() {
  ChatController::Instance().Shutdown();
}

} // namespace pbr
