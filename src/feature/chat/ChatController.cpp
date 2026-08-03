#include <stdexcept>
#include "feature/chat/ChatController.h"
#include "feature/messaging/MessagingChatPorts.h"
#include "feature/ui/ShellSetupPorts.h"
#include "feature/chat/ChatDataModel.h"
#include "feature/chat/ChatWidgetHost.h"
#include "feature/ui/BadgeAggregator.h"
#include "base/i18n/LocalizationService.h"
#include "base/i18n/ScriptLanguageDetector.h"
#include "base/platform/AppLifecycle.h"
#include "base/platform/BackgroundSyncScheduler.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/DesktopWindowChrome.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPushDeviceRegistrar.h"

#include "base/ai/StructuredTextParser.h"
#include "base/ai/WorkingSetPolicy.h"
#include "base/ai/conversation/Conversation.h"
#include "base/platform/IAssetLocator.h"
#include "base/ui/ContextMenuHost.h"
#include "base/ui/InputCoordinator.h"
#include "base/ui/ChatFormHelper.h"
#include "base/ui/RmlVariantHelpers.h"
#include "feature/chat/CalendarHelper.h"
#include "feature/chat/ChatWidgetStateBuilder.h"
#include "common/Utilities.h"
#include "common/StartupTiming.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/people/ContactJson.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/messaging/AtAiParser.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "base/messaging/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/SendRelayOptions.h"
#include "base/messaging/SyncStateTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DocumentLoader.h"
#include "feature/ui/DeferredStartup.h"
#include "base/crypto/ProfileUnlockGate.h"
#include "feature/ui/CallController.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/UserFeedback.h"
#include "libp2p/integration/host/Reachability.h"
#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/PeoplePickerController.h"

#include <RmlUi/Core/SystemInterface.h>
#include "feature/ui/SettingsController.h"

#include <nlohmann/json.hpp>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/SystemInterface.h>

#include <optional>
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

std::string MockAssistantRespond(const std::string& query) {
  const std::string lower = util::ToLowerAscii(query);

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

} // namespace

ChatController::ChatController()
    : scroller_(context_,
                ChatTranscriptScroller::View{
                    .show_jump_to_latest = chat_.show_jump_to_latest,
                    .jump_to_latest_label = chat_.jump_to_latest_label,
                    .messages = chat_.messages,
                    .has_turns = chat_.has_turns,
                },
                messaging_ready_),
      working_set_(WorkingSetController::ShellView{
          .working_set_active = shell_.working_set_active,
          .working_set_title = shell_.working_set_title,
          .working_set_subtitle = shell_.working_set_subtitle,
          .working_set_rml = shell_.working_set_rml,
          .working_set = shell_.working_set,
      }),
      chrome_(ChatThreadChrome::View{
                 .draft_placeholder = chat_.draft_placeholder,
                 .status = chat_.status,
                 .thread_title = chat_.thread_title,
                 .thread_subtitle = chat_.thread_subtitle,
                 .peer_link_status = chat_.peer_link_status,
                 .peer_link_banner = chat_.peer_link_banner,
                 .show_peer_link = chat_.show_peer_link,
                 .show_peer_link_banner = chat_.show_peer_link_banner,
                 .show_retry_peer_dial = chat_.show_retry_peer_dial,
                 .thread_encrypted = chat_.thread_encrypted,
                 .thread_is_ai = chat_.thread_is_ai,
                 .thread_is_private = chat_.thread_is_private,
                 .thread_is_public = chat_.thread_is_public,
                 .thread_is_group = chat_.thread_is_group,
                 .compose_disabled = chat_.compose_disabled,
                 .show_thread_actions = chat_.show_thread_actions,
                 .show_peer_sheet = chat_.show_peer_sheet,
                 .show_call_actions = chat_.show_call_actions,
                 .show_forget_memory = chat_.show_forget_memory,
                 .show_sync_with_peer = chat_.show_sync_with_peer,
                 .show_thread_menu = chat_.show_thread_menu,
                 .show_gap_banner = chat_.show_gap_banner,
                 .show_compromised_banner = chat_.show_compromised_banner,
                 .show_psk_setup_banner = chat_.show_psk_setup_banner,
                 .show_psk_import = chat_.show_psk_import,
                 .psk_has_key = chat_.psk_has_key,
                 .psk_verified = chat_.psk_verified,
                 .psk_fingerprint = chat_.psk_fingerprint,
                 .psk_export_b64 = chat_.psk_export_b64,
                 .psk_import_text = chat_.psk_import_text,
                 .sync_in_progress = chat_.sync_in_progress,
                 .show_older_history_hint = chat_.show_older_history_hint,
                 .turns = chat_.turns,
                 .messages = chat_.messages,
                 .use_messages_layout = chat_.use_messages_layout,
                 .has_turns = chat_.has_turns,
             },
             messaging_ready_) {
  redirectLogger("ChatController");
  scroller_.SetDirtyTurns([]() { DirtyChatTurns(); });
  working_set_.SetWidgetLookup([this](const std::string& entry_id) -> const TurnWidgetState* {
    return widgets_.Find(entry_id);
  });
  chrome_.SetRefreshFromMessaging([this]() { RefreshFromMessaging(); });
  chrome_.SetWithSecrets([this](std::function<void()> action) { WithSecrets(std::move(action)); });
  chrome_.SetOnScrollerReset([this]() { scroller_.Reset(); });
  chrome_.SetCaptureScrollBeforePrepend([this]() { scroller_.CaptureScrollBeforePrependIfUnpinned(); });
  chrome_.SetExpandLoadedMinFromOlderPage(
      [this](const std::string& thread_id, int64_t before) {
        scroller_.ExpandLoadedMinFromOlderPage(thread_id, before);
      });
}

ChatController& ChatController::Instance() {
  static ChatController controller;
  return controller;
}
void ChatController::BindChatPorts(MessagingChatPorts ports) {
  chat_ports_ = std::move(ports);
  scroller_.BindChatPorts(chat_ports_);
  chrome_.BindChatPorts(chat_ports_);
}

void ChatController::BindAgentPorts(AgentUiPorts ports) {
  agent_ports_ = std::move(ports);
}

bool ChatController::AgentReady() const {
  return agent_ports_.has_session && agent_ports_.has_session();
}

bool ChatController::AgentConfigured() const {
  if (agent_ports_.snapshot) {
    return agent_ports_.snapshot().configured;
  }
  return false;
}

void ChatController::BindShellSetup(ShellSetupPorts ports) {
  shell_setup_ = std::move(ports);
}

bool ChatController::MessagingInitialized() const {
  if (chat_ports_.snapshot) {
    return chat_ports_.snapshot().initialized;
  }
  if (messaging_ui_.snapshot) {
    return messaging_ui_.snapshot().initialized;
  }
  return false;
}

bool ChatController::MessagingReady() const {
  if (chat_ports_.snapshot) {
    return chat_ports_.snapshot().messaging_ready;
  }
  if (messaging_ui_.snapshot) {
    return messaging_ui_.snapshot().messaging_ready;
  }
  return false;
}

const std::string& ChatController::ActiveThreadId() const {
  static const std::string kEmpty;
  if (chat_ports_.active_thread_id) {
    return chat_ports_.active_thread_id();
  }
  return kEmpty;
}

void ChatController::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
}

void ChatController::BindBadgeAggregator(BadgeAggregator& badges) {
  badges_ = &badges;
}

void ChatController::BindInputCoordinator(InputCoordinator& input) {
  input_ = &input;
}

void ChatController::BindCallController(CallController& call) {
  call_ = &call;
}

void ChatController::BindUnlockGate(ProfileUnlockGate& unlock_gate) {
  unlock_gate_ = &unlock_gate;
}

void ChatController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
  chrome_.BindShellNavigation(shell_navigation_);
  working_set_.BindShellNavigation(shell_navigation_);
}

void ChatController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
  chrome_.BindShellFeedback(shell_feedback_);
}

void ChatController::BindMessagingUi(MessagingUiPorts ports) {
  messaging_ui_ = std::move(ports);
}

ShellChromeSnapshot ChatController::ChromeSnapshot() const {
  return shell_navigation_.snapshot ? shell_navigation_.snapshot() : ShellChromeSnapshot{};
}

void ChatController::ShellDirty() {
  if (shell_navigation_.dirty_window) {
    shell_navigation_.dirty_window();
  }
}

void ChatController::ShellSyncLayout(const bool restore_focus_after) {
  if (shell_navigation_.request_sync_layout) {
    shell_navigation_.request_sync_layout(restore_focus_after, nullptr);
  }
}

void ChatController::ShellSelectNavTab(const NavTab tab) {
  if (shell_navigation_.select_nav_tab) {
    shell_navigation_.select_nav_tab(tab);
  }
}

void ChatController::ShellSetPrimaryPane(const std::string& key) {
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane(key);
  }
}

void ChatController::ShellOpenCompactChat() {
  if (shell_navigation_.open_compact_chat) {
    shell_navigation_.open_compact_chat();
  }
}

void ChatController::ShellCloseCompactChat() {
  if (shell_navigation_.close_compact_chat) {
    shell_navigation_.close_compact_chat();
  }
}

void ChatController::ShellSetActivity(const bool visible, const Rml::String& message) {
  if (shell_navigation_.set_activity) {
    shell_navigation_.set_activity(visible, message);
  }
}

void ChatController::ShellRemountNavRail() {
  if (shell_navigation_.request_remount_nav_rail) {
    shell_navigation_.request_remount_nav_rail();
  }
}

void ChatController::ShowToast(const std::string& message, const ToastDuration duration) {
  if (shell_feedback_.show_toast) {
    shell_feedback_.show_toast(message, duration);
  }
}

void ChatController::ShowConfirm(const std::string& title, const std::string& message,
                                 std::function<void(bool)> on_result) {
  if (shell_feedback_.show_confirm) {
    shell_feedback_.show_confirm(title, message, std::move(on_result));
  }
}

void ChatController::ShowConfirmWithCheckbox(const std::string& title, const std::string& message,
                                             const std::string& checkbox_label, const bool checkbox_default,
                                             std::function<void(bool, bool)> on_result) {
  if (shell_feedback_.show_confirm_with_checkbox) {
    shell_feedback_.show_confirm_with_checkbox(title, message, checkbox_label, checkbox_default, std::move(on_result));
  }
}

void ChatController::ShowPrompt(const std::string& title, const std::string& message, const std::string& default_value,
                              std::function<void(bool, std::string)> on_result) {
  if (shell_feedback_.show_prompt) {
    shell_feedback_.show_prompt(title, message, default_value, std::move(on_result));
  }
}

SessionStore& ChatController::Store() {
  if (!session_store_) {
    throw std::runtime_error("ChatController session store not bound");
  }
  return *session_store_;
}

const SessionStore& ChatController::Store() const {
  if (!session_store_) {
    throw std::runtime_error("ChatController session store not bound");
  }
  return *session_store_;
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
  Instance().working_set_.Open(std::string(args[0].Get<Rml::String>().c_str()), *block_index);
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

void ChatController::StartVoiceCallCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  ChatController& self = Instance();
  const std::string thread_id = self.ActiveThreadId();
  if (!thread_id.empty() && self.call_) {
    self.call_->StartVoiceCall(thread_id);
  }
}

void ChatController::StartVideoCallCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  ChatController& self = Instance();
  const std::string thread_id = self.ActiveThreadId();
  if (!thread_id.empty() && self.call_) {
    self.call_->StartVideoCall(thread_id);
  }
}

void ChatController::OpenPeerSheetCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnOpenPeerSheet(ev);
}

void ChatController::SelectThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/, const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnSelectThread(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::CloseThreadCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev, const Rml::VariantList& args) {
  ev.StopPropagation();
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
  working_set_.ClearAll();
  RefreshFromMessaging();
  // Always land on latest when opening/showing a thread (incl. re-open same id).
  scroller_.RequestScrollToLatest();
  if (ChromeSnapshot().layout_mode == LayoutMode::Compact &&
      ChromeSnapshot().nav_tab == NavTab::Sessions) {
    ShellOpenCompactChat();
  }
}

void ChatController::OnHomeTabActivated() {
  if (!messaging_ready_) {
    return;
  }
  chat_ports_.clear_active_thread();
  working_set_.ClearAll();
  ShellSetPrimaryPane("home");
  RefreshFromMessaging();
  ShellSyncLayout();
  ShellDirty();
}

void ChatController::OnSessionsTabActivated() {
  working_set_.ClearAll();
}

void ChatController::OnSelectThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  if (chat_ports_.open_thread(thread_id)) {
    ILocalNotifier::Instance().ClearForThread(thread_id);
    chat_ports_.maybe_tail_sync(thread_id);
    ShellSetPrimaryPane("chat");
    FinalizeThreadDisplay();
  }
}

void ChatController::OnCloseThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }

  auto finish_close = [this, thread_id]() {
    if (!chat_ports_.close_thread(thread_id)) {
      UserFeedback::Fail("Could not delete conversation");
      ShellDirty();
      return;
    }
    chat_.draft = "";
    chat_.status = "";
    chat_.loading = false;
    pending_reply_.reset();
    working_set_.ClearAll();
    widgets_.ClearAll();
    chat_.turns.clear();
    RefreshFromMessaging();
    if (shell_.sessions.empty()) {
      ShellSelectNavTab(NavTab::Home);
      ShellCloseCompactChat();
    }
    ShellSyncLayout();
    ShellDirty();
  };

  auto dismiss_and_close = [this, finish_close](const std::string& group_id) {
    (void)chat_ports_.dismiss_local_group(group_id);
    finish_close();
  };

  auto thread = chat_ports_.get_thread(thread_id);
  if (thread && *thread && (*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    const std::string group_id = *(*thread)->group_id;
    std::string local_identity;
    if (auto identity = chat_ports_.get_identity()) {
      local_identity = identity->relay_user_id;
    }

    bool is_owner = false;
    if (auto owner = chat_ports_.is_local_owner(group_id)) {
      is_owner = *owner;
    } else if (auto roster = chat_ports_.list_group_roster(group_id)) {
      for (const GroupRosterMember& member : *roster) {
        if (member.member_identity == local_identity && member.role == MemberRole::Owner) {
          is_owner = true;
          break;
        }
      }
    }

    std::vector<GroupRosterMember> members;
    if (auto roster = chat_ports_.list_group_roster(group_id)) {
      members = *roster;
    }

    std::vector<GroupRosterMember> successors;
    for (const GroupRosterMember& member : members) {
      if (member.member_identity == local_identity) {
        continue;
      }
      if (chat_ports_.is_member_unreachable(group_id, member.member_identity)) {
        continue;
      }
      successors.push_back(member);
    }

    bool owner_on_roster = false;
    if (auto owner_id = chat_ports_.owner_identity(group_id)) {
      for (const GroupRosterMember& member : members) {
        if (member.member_identity == *owner_id) {
          owner_on_roster = true;
          break;
        }
      }
    }

    // Not the owner: normal leave — unless this is an orphaned/solo shell (owner missing or no
    // reachable peers), then dismiss locally. Covers "both sides solo, neither is owner".
    if (!is_owner) {
      const bool orphaned_shell = successors.empty() || !owner_on_roster;
      if (orphaned_shell) {
        ShowConfirm(Tr("chat.group.dismiss_title"),
                                   Tr("chat.group.dismiss_confirm"), [dismiss_and_close, group_id](bool ok) {
                                     if (!ok) {
                                       return;
                                     }
                                     dismiss_and_close(group_id);
                                   });
      } else {
        ShowConfirm(Tr("chat.group.leave_title"),
                                   Tr("chat.group.leave_confirm"),
                                   [this, finish_close, dismiss_and_close, group_id](bool ok) {
                                     if (!ok) {
                                       return;
                                     }
                                     if (auto left = chat_ports_.leave_group(group_id); !left) {
                                       dismiss_and_close(group_id);
                                       return;
                                     }
                                     finish_close();
                                   });
      }
      return;
    }

    // Solo or only-unreachable peers: local dismiss — no transfer into a ghost roster.
    if (successors.empty()) {
      ShowConfirm(Tr("chat.group.dismiss_title"),
                                 Tr("chat.group.dismiss_confirm"), [dismiss_and_close, group_id](bool ok) {
                                   if (!ok) {
                                     return;
                                   }
                                   dismiss_and_close(group_id);
                                 });
      return;
    }

    ShowConfirm(Tr("chat.group.leave_title"), Tr("chat.group.leave_owner_confirm"),
        [this, finish_close, dismiss_and_close, group_id, successors](bool ok) {
          if (!ok) {
            return;
          }
          std::vector<ContextMenuAction> actions;
          for (const GroupRosterMember& member : successors) {
            std::string label = member.member_identity;
            if (auto contact = chat_ports_.find_contact_by_identity(member.member_identity,
                                                                                  ContactIdKind::RelayUser)) {
              if (*contact) {
                label = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
                if (label.empty()) {
                  label = member.member_identity;
                }
              }
            }
            const std::string successor = member.member_identity;
            actions.push_back({
                "transfer_" + successor,
                "Transfer to " + label,
                nullptr,
                [this, finish_close, group_id, successor]() {
                  if (auto left = chat_ports_.leave_as_owner(group_id, successor); !left) {
                    UserFeedback::Fail(left.error().message);
                    ShellDirty();
                    return;
                  }
                  finish_close();
                },
                "../icons/contacts.svg",
            });
          }
          actions.push_back({
              "dismiss_local",
              "Dismiss on this device only",
              nullptr,
              [dismiss_and_close, group_id]() { dismiss_and_close(group_id); },
              "../icons/trash.svg",
              true,
          });
          ContextMenuHost::Instance().ShowActions(Rml::Vector2i(120, 120), std::move(actions));
          ShellDirty();
        });
    return;
  }

  ShowConfirm(Tr("chat.delete_conversation"),
                             Tr("chat.delete_confirm"), [finish_close](bool ok) {
                               if (!ok) {
                                 return;
                               }
                               finish_close();
                             });
}

void ChatController::OnClearHistory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  auto thread = chat_ports_.get_active_thread();
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
    ShowConfirmWithCheckbox(Tr("chat.clear_history"), message, "Also forget what AI learned", false,
        [this, thread_id](bool ok, bool forget_memory) {
          if (!ok) {
            return;
          }
          if (!chat_ports_.clear_thread_history(thread_id, forget_memory)) {
            return;
          }
          chat_.draft = "";
          chat_.status = "";
          chat_.loading = false;
          pending_reply_.reset();
          widgets_.ClearAll();
          RefreshFromMessaging();
          ShellDirty();
        });
  } else {
    ShowConfirm(Tr("chat.clear_history"), message,
                               [this, thread_id](bool ok) {
                                 if (!ok) {
                                   return;
                                 }
                                 if (!chat_ports_.clear_thread_history(thread_id, false)) {
                                   return;
                                 }
                                 chat_.draft = "";
                                 chat_.status = "";
                                 chat_.loading = false;
                                 pending_reply_.reset();
                                 widgets_.ClearAll();
                                 RefreshFromMessaging();
                                 ShellDirty();
                               });
  }
}

void ChatController::OnForgetMemory() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShowConfirm("Forget what AI learned?",
      "Delete the durable conversation summary? Your message transcript stays on this device.",
      [this, thread_id](bool ok) {
        if (!ok) {
          return;
        }
        if (!chat_ports_.forget_thread_memory(thread_id)) {
          return;
        }
        ShellDirty();
      });
}

void ChatController::LoadOlderHistoryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnLoadOlderHistory();
}

void ChatController::RetryPeerDialCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                           const Rml::VariantList& /*args*/) {
  Instance().OnRetryPeerDial();
}

void ChatController::SendSharedAssistantRelay(const std::string& thread_id, const AtAiMode mode,
                                              const std::string& plain_text) {
  if (!messaging_ready_ || plain_text.empty()) {
    return;
  }
  auto thread = chat_ports_.get_thread(thread_id);
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
  (void)chat_ports_.send_user_message(thread_id, plain_text, opts);
}

void ChatController::RefreshFromMessaging() {
  const int prev_sessions = ChromeSnapshot().nav_badges.sessions_unread;
  const int prev_contacts = ChromeSnapshot().nav_badges.contacts_unread;
  SyncShellSessions();
  SyncDisplayFromThread();
  chrome_.Update();
  if (badges_) {
    badges_->Refresh();
  }
  // Inbox ingest (incl. call_invite) completes on IO; reconcile ring after messages change.
  if (call_) {
    call_->RefreshPendingRing();
  }
  DirtyChat();
  DirtyShell();
  ShellDirty();
  const NavBadgeState& badges = ChromeSnapshot().nav_badges;
  if (badges.sessions_unread != prev_sessions || badges.contacts_unread != prev_contacts) {
    ShellRemountNavRail();
  }
}

void ChatController::OnProfileDataReset() {
  messaging_ready_ = false;
  working_set_.ClearAll();
  widgets_.ClearAll();
  pending_reply_.reset();
  chrome_.ResetPanelState();
  if (MessagingInitialized()) {
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
  auto threads = chat_ports_.list_threads();
  if (!threads) {
    return;
  }
  std::vector<Thread> sorted_threads = *threads;
  std::sort(sorted_threads.begin(), sorted_threads.end(),
            [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });

  const std::string active_id = ActiveThreadId();
  for (const Thread& thread : sorted_threads) {
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = chat_ports_.resolve_thread_label
                    ? chat_ports_.resolve_thread_label(thread).title.c_str()
                    : thread.title.c_str();
    row.preview = thread.preview.c_str();
    row.kind = SessionVisualKind(thread);
    row.unread_count = thread.unread_count;
    row.unread_display = FormatBadgeCount(thread.unread_count).c_str();
    row.active = thread.id == active_id;
    row.closable = true;
    shell_.sessions.push_back(std::move(row));
  }
}

void ChatController::OnMessagesScroll() {
  scroller_.OnMessagesScroll();
}

void ChatController::OnJumpToLatest() {
  scroller_.OnJumpToLatest();
}

void ChatController::MessagesScrollCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnMessagesScroll();
}

void ChatController::JumpToLatestCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  Instance().OnJumpToLatest();
}

void ChatController::OnRetryPeerDial() {
  chrome_.OnRetryPeerDial();
}

void ChatController::OnLoadOlderHistory() {
  chrome_.OnLoadOlderHistory();
}

void ChatController::OnSyncWithPeer() {
  chrome_.OnSyncWithPeer();
}

void ChatController::OnRetryGapSync() {
  chrome_.OnRetryGapSync();
}

void ChatController::OnStartNewSecureChat() {
  chrome_.OnStartNewSecureChat();
}

void ChatController::OnPauseIntegrityOnly() {
  chrome_.OnPauseIntegrityOnly();
}

void ChatController::OnCopyPskKey() {
  chrome_.OnCopyPskKey();
}

void ChatController::OnTogglePskImport() {
  chrome_.OnTogglePskImport();
}

void ChatController::OnImportPsk() {
  chrome_.OnImportPsk();
}

void ChatController::OnVerifyPsk() {
  chrome_.OnVerifyPsk();
}

void ChatController::OnRotatePskExport() {
  chrome_.OnRotatePskExport();
}

void ChatController::UpdateThreadChrome() {
  chrome_.Update();
}

void ChatController::UpdatePeerLinkChrome() {
  chrome_.UpdatePeerLink();
}

void ChatController::ResetChatPanelState() {
  chrome_.ResetPanelState();
}

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  if (!chat_ports_.get_active_thread || !chat_ports_.get_active_thread()) {
    chrome_.ResetPanelState();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id);

  const std::string prev_tail_id =
      chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
  const size_t prev_count = chat_.messages.size();

  chat_.messages = chat_ports_.build_display_rows
      ? chat_ports_.build_display_rows(thread_id, scroller_.LoadedMinDisplayOrder())
      : std::vector<MessageDisplayRow>{};
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;

  scroller_.EndDisplaySync(thread_changed, prev_tail_id, prev_count);
}

void ChatController::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    const nlohmann::json action_json = nlohmann::json::parse(*payload, nullptr, false);
    if (action_json.is_object() && action_json.contains("type") && action_json["type"].is_string() &&
        action_json["type"].get<std::string>() == "fork_group") {
      const std::string confirmed_payload = *payload;
      ShowConfirm("Start a new group?",
          "This creates a new group with a fresh history. People who are still reachable can be invited again.",
          [this, message, confirmed_payload](bool ok) {
            if (!ok) {
              return;
            }
            auto result = chat_ports_.dispatch_action(confirmed_payload);
            if (!result) {
              log().warning << "Local action failed: " << result.error().message;
              ShowToast(result.error().message);
              ShellDirty();
              return;
            }
            if (*result) {
              SendUserText(message, *result);
              return;
            }
            RefreshFromMessaging();
            ContactsController::Instance().Refresh();
            if (!ActiveThreadId().empty()) {
              ShellSelectNavTab(NavTab::Sessions);
              ShellSetPrimaryPane("chat");
              if (ChromeSnapshot().layout_mode == LayoutMode::Compact) {
                ShellOpenCompactChat();
              }
            }
            ShellDirty();
          });
      return;
    }
    auto result = chat_ports_.dispatch_action(*payload);
    if (!result) {
      // Never re-route the same action payload through MessageRouter — that loops
      // HandleLocalAction → SendUserText → Route → HandleLocalAction until stack overflow.
      log().warning << "Local action failed: " << result.error().message;
      ShowToast(result.error().message);
      ShellDirty();
      return;
    }
    if (*result) {
      SendUserText(message, *result);
      return;
    }
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
    if (!ActiveThreadId().empty()) {
      ShellSelectNavTab(NavTab::Sessions);
      ShellSetPrimaryPane("chat");
      if (ChromeSnapshot().layout_mode == LayoutMode::Compact) {
        ShellOpenCompactChat();
      }
    }
    return;
  }
  SendUserText(message, payload);
}

void ChatController::OnSendMessage() {
  if (chat_.loading || chat_.compose_disabled) {
    return;
  }

  const std::string text = util::Trim(chat_.draft.c_str());
  if (text.empty()) {
    return;
  }

  if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
    ShowToast("Message is too long to send.");
    return;
  }

  chat_.draft = "";
  DirtyChatChrome();
  scroller_.RequestScrollToLatest();
  SendUserText(text);
}

void ChatController::OnNewChat() {
  if (!messaging_ready_) {
    return;
  }

  (void)chat_ports_.create_new_ai_thread();
  chat_.draft = "";
  chat_.status = "";
  chat_.loading = false;
  chat_.compose_disabled = false;
  pending_reply_.reset();
  if (AgentReady() && agent_ports_.cancel) {
    agent_ports_.cancel();
  }
  working_set_.ClearAll();
  widgets_.ClearAll();
  ShellSetPrimaryPane("chat");
  focus_draft_after_sync_ = true;
  FinalizeThreadDisplay();
  ShellDirty();
}

void ChatController::OnNewMessage() {
  PeoplePickerController::Instance().OpenFree();
}

void ChatController::OnOpenNewSessionMenu(Rml::Event& ev) {
  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      "Chat with AI",
      nullptr,
      [this]() { OnNewChat(); },
      "../icons/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      "Message a contact",
      nullptr,
      [this]() { OnNewMessage(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      "Find someone",
      nullptr,
      [this]() { OnFindSomeone(); },
      "../icons/message.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenThreadActionsMenu(Rml::Event& ev) {
  // Anchor near the right edge of the trigger so the menu stays in the header corner.
  const Rml::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (chat_.show_sync_with_peer && !chat_.sync_in_progress) {
    actions.push_back({
        "sync_with_peer",
        "Sync with peer",
        nullptr,
        [this]() { OnSyncWithPeer(); },
        "../icons/sync.svg",
    });
  }
  if (chat_.show_thread_actions) {
    actions.push_back({
        "clear_history",
        "Clear history…",
        nullptr,
        [this]() { OnClearHistory(); },
        "../icons/trash.svg",
        true,
    });
  }
  if (chat_.show_forget_memory) {
    actions.push_back({
        "forget_memory",
        "Forget AI memory…",
        nullptr,
        [this]() { OnForgetMemory(); },
        "../icons/sparkle.svg",
        true,
    });
  }
  if (actions.empty()) {
    return;
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenPeerSheet(Rml::Event& ev) {
  if (!messaging_ready_ || !chat_.show_peer_sheet) {
    return;
  }
  auto thread = chat_ports_.get_active_thread();
  if (!thread) {
    return;
  }

  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (thread->kind == ThreadKind::Direct) {
    const PeerDisplayLabel label = chat_ports_.resolve_thread_label(*thread);
    const std::string peer_id = thread->peer_identity_value;
    std::optional<std::string> dm_contact_id = label.contact_id;
    if (!dm_contact_id && !thread->participant_contact_ids.empty()) {
      const std::string& candidate = thread->participant_contact_ids.front();
      if (!candidate.empty()) {
        if (auto contact = chat_ports_.get_contact(candidate); contact && *contact) {
          dm_contact_id = candidate;
        }
      }
    }
    if (dm_contact_id) {
      const std::string contact_id = *dm_contact_id;
      actions.push_back({
          "add_people",
          "Add people…",
          nullptr,
          [contact_id]() { PeoplePickerController::Instance().OpenFromDm(contact_id); },
          "../icons/group.svg",
      });
      actions.push_back({
          "view_contact",
          "View contact",
          nullptr,
          [contact_id]() { ContactsController::Instance().OnSelectContact(contact_id); },
          "../icons/contacts.svg",
      });
    } else if (!peer_id.empty()) {
      actions.push_back({
          "add_contact",
          "Add to contacts",
          nullptr,
          [this, peer_id]() {
            DirectoryHit hit;
            if (auto shadow = chat_ports_.get_directory_shadow(peer_id)) {
              hit = *shadow;
            } else {
              hit.hit_id = peer_id;
              hit.ids = {{ContactIdKind::RelayUser, peer_id, true}};
            }
            auto created = chat_ports_.add_contact_from_directory_hit(hit);
            if (!created) {
              UserFeedback::Fail("Could not add contact");
              ShellDirty();
              return;
            }
            if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
              chat_ports_.register_peer_signing_key(ContactIdKindToString(ContactIdKind::RelayUser), peer_id,
                                                 *hit.signing_public_key_b64, "directory");
            }
            if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
              chat_ports_.register_peer_kem_key(ContactIdKindToString(ContactIdKind::RelayUser), peer_id,
                                             *hit.kem_public_key_b64, "directory");
            }
            chat_ports_.register_contact_direct_endpoints(*created);
            // Bind stranger DM (empty participants) to the new contact id.
            ThreadChannel channel = ThreadChannel::E2ePublic;
            if (auto active = chat_ports_.get_active_thread();
                active && active->kind == ThreadKind::Direct) {
              channel = active->channel;
            }
            (void)chat_ports_.find_or_create_direct_thread(created->id, channel);
            ContactsController::Instance().OnSelectContact(created->id);
            chat_ports_.notify_thread_changed();
          },
          "../icons/contacts.svg",
      });
    }
    if (!peer_id.empty()) {
      actions.push_back({
          "copy_id",
          "Copy ID",
          nullptr,
          [this, peer_id]() {
            if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
              system->SetClipboardText(peer_id.c_str());
            }
            ShowToast("ID copied");
            ShellDirty();
          },
          "../icons/copy.svg",
      });
    }
  } else if (thread->kind == ThreadKind::Group && thread->group_id) {
    const std::string thread_id = thread->id;
    const std::string group_id = *thread->group_id;
    const std::string current_local = thread->local_title;
    const PeerDisplayLabel label = chat_ports_.resolve_thread_label(*thread);
    const std::string shared_default = label.shared_title.value_or(thread->title);

    actions.push_back({
        "rename_for_me",
        "Rename for me",
        nullptr,
        [this, thread_id, current_local, shared_default]() {
          ShowPrompt("Rename for me", "Local nickname for this group",
              current_local.empty() ? shared_default : current_local,
              [this, thread_id](bool ok, std::string value) {
                if (!ok) {
                  return;
                }
                if (auto saved = chat_ports_.set_thread_local_title(thread_id, value); !saved) {
                  UserFeedback::Fail(saved.error().message);
                }
                ShellDirty();
              });
        },
        "../icons/message.svg",
    });
    if (!current_local.empty()) {
      actions.push_back({
          "clear_my_name",
          "Clear my name",
          nullptr,
          [this, thread_id]() {
            (void)chat_ports_.set_thread_local_title(thread_id, "");
            ShellDirty();
          },
          "../icons/trash.svg",
      });
    }

    bool is_owner = false;
    std::string local_identity;
    if (auto identity = chat_ports_.get_identity()) {
      local_identity = identity->relay_user_id;
      if (auto roster = chat_ports_.list_group_roster(group_id)) {
        for (const auto& member : *roster) {
          if (member.member_identity == identity->relay_user_id && member.role == MemberRole::Owner) {
            is_owner = true;
            break;
          }
        }
      }
    }
    const bool owner_unreachable = chat_ports_.is_owner_unreachable(group_id);
    if (is_owner) {
      actions.push_back({
          "rename_for_everyone",
          "Rename for everyone",
          nullptr,
          [this, group_id, shared_default]() {
            ShowPrompt("Rename for everyone", "Shared group name for all members",
                shared_default, [this, group_id](bool ok, std::string value) {
                  if (!ok) {
                    return;
                  }
                  if (value.empty()) {
                    UserFeedback::Fail("Title required");
                    ShellDirty();
                    return;
                  }
                  if (auto renamed = chat_ports_.rename_group_shared(group_id, value); !renamed) {
                    UserFeedback::Fail(renamed.error().message);
                  } else {
                    chat_ports_.notify_thread_changed();
                  }
                  ShellDirty();
                });
          },
          "../icons/group.svg",
      });
      for (const std::string& unreachable_id : chat_ports_.list_unreachable_members(group_id)) {
        if (unreachable_id == local_identity) {
          continue;
        }
        std::string name = unreachable_id;
        if (auto contact =
                chat_ports_.find_contact_by_identity(unreachable_id, ContactIdKind::RelayUser)) {
          if (*contact) {
            name = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
            if (name.empty()) {
              name = unreachable_id;
            }
          }
        }
        actions.push_back({
            "remove_unreachable_" + unreachable_id,
            "Remove unreachable: " + name,
            nullptr,
            [this, group_id, unreachable_id, name]() {
              ShowConfirm("Remove member",
                  "Remove " + name + " from this group? Others will be notified.",
                  [this, group_id, unreachable_id](bool ok) {
                    if (!ok) {
                      return;
                    }
                    if (auto removed =
                            chat_ports_.remove_member_by_identity(group_id, unreachable_id);
                        !removed) {
                      UserFeedback::Fail(removed.error().message);
                    } else {
                      chat_ports_.notify_thread_changed();
                    }
                    ShellDirty();
                  });
            },
            "../icons/trash.svg",
            true,
        });
      }
    } else if (owner_unreachable) {
      actions.push_back({
          "rename_for_everyone",
          "Rename for everyone",
          nullptr,
          [this]() {
            ShowToast("Only the owner can do that — see the note in the chat");
            ShellDirty();
          },
          "../icons/group.svg",
      });
    }
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
  // SyncLayout remounts the shell DOM and resets scroll offsets — re-arm follow-tail.
  scroller_.OnShellRemounted();
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

void ChatController::SubmitForm(const std::string& entry_id, const std::string& form_id) {
  if (chat_.loading) {
    return;
  }
  const auto submission = widgets_.TrySubmit(entry_id, form_id);
  if (!submission) {
    log().warning << "Ignoring submit for inactive or expired form: " << entry_id << "/" << form_id;
    return;
  }
  working_set_.Clear();
  SyncDisplayFromThread();
  SendUserText(submission->display_text, submission->payload);
}

void ChatController::CalendarPrev(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, -1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::CalendarNext(const std::string& entry_id) {
  if (!widgets_.ShiftCalendar(entry_id, 1)) {
    return;
  }
  if (working_set_.ActiveEntryId() == entry_id) {
    working_set_.SyncWidgetBindings(entry_id);
  }
  SyncDisplayFromThread();
}

void ChatController::SelectCalendarDay(const std::string& entry_id, const std::string& iso_date) {
  if (chat_.loading) {
    return;
  }
  if (!widgets_.IsCalendarDayAvailable(entry_id, iso_date)) {
    return;
  }
  SendUserText("Selected " + iso_date);
}

std::string ChatController::HydrateAssistantRml(const TranscriptEntry& entry) const {
  return widgets_.HydrateAssistantRml(entry);
}

bool ChatController::IsFormEditable(const std::string& entry_id, const std::string& form_id) const {
  return widgets_.IsFormEditable(entry_id, form_id);
}

void ChatController::InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits) {
  widgets_.Initialize(entry_id, inits);
}

void ChatController::MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const {
  widgets_.MergeIntoRow(entry_id, row);
}

TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) {
  return widgets_.Find(entry_id);
}

const TurnWidgetState* ChatController::FindWidgetState(const std::string& entry_id) const {
  return widgets_.Find(entry_id);
}

void ChatController::ClearFormState() {
  widgets_.ClearForms();
}

void ChatController::UpdateSidebarPreview(const std::string& preview_text) {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  (void)chat_ports_.update_preview(thread_id, preview_text);
  SyncShellSessions();
  DirtyShell();
}

bool ChatController::EnsureHomeOutboundSession() {
  if (!messaging_ready_) {
    return false;
  }
  if (!chat_ports_.create_new_ai_thread()) {
    return false;
  }
  ShellSelectNavTab(NavTab::Sessions);
  ShellSetPrimaryPane("chat");
  FinalizeThreadDisplay();
  ShellDirty();
  return true;
}

void ChatController::SendUserText(const std::string& text, std::optional<std::string> user_payload) {
  const std::string trimmed = util::Trim(text);
  if (trimmed.empty() || chat_.loading) {
    return;
  }
  if (chat_.compose_disabled && messaging_ready_) {
    return;
  }
  if (!messaging_ready_) {
    WithSecrets([this, trimmed, user_payload = std::move(user_payload)]() mutable {
      SendUserText(trimmed, std::move(user_payload));
    });
    return;
  }

  if (ChromeSnapshot().nav_tab == NavTab::Home) {
    if (!EnsureHomeOutboundSession()) {
      return;
    }
  }

  widgets_.ExpireOpenForms();
  DirtyChatTurns();

  const bool use_mock_reply = !use_llm_;
  bool expect_agent_work = use_mock_reply;
  if (!expect_agent_work && messaging_ready_ && chat_ports_.has_router && chat_ports_.has_router()) {
    expect_agent_work = chat_ports_.expects_agent_work(
        ActiveThreadId(), trimmed, user_payload);
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
    const std::string thread_id = ActiveThreadId();
    ThreadMessage user_message;
    user_message.id = util::GenerateUuid();
    user_message.thread_id = thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    user_message.text = trimmed;
    user_message.timestamp = util::NowUnixMs();
    user_message.transport = MessageTransport::Local;
    (void)chat_ports_.append_message(user_message);
    SyncDisplayFromThread();
    DirtyChatTurns();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = user_message.id, .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  if (messaging_ready_ && chat_ports_.has_router && chat_ports_.has_router()) {
    log().info << "Routing message via MessageRouter";
    (void)chat_ports_.route_message(ActiveThreadId(), trimmed,
                                                  std::move(user_payload));
    SyncDisplayFromThread();
    return;
  }

  log().info << "Submitting message to agent session";
  if (agent_ports_.submit) {
    agent_ports_.submit(trimmed, std::move(user_payload));
  }
}

void ChatController::SendChatAction(const std::string& entry_id, int action_index) {
  if (chat_.loading || action_index < 0 || !messaging_ready_) {
    return;
  }

  const std::string thread_id = ActiveThreadId();
  auto messages = chat_ports_.get_messages_page(thread_id, std::nullopt, 10000);
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
    // Copy by value: layout-mutating actions close the working set / remount panes. Doing that
    // synchronously destroys the click target and leaves RmlUi data views with dangling aliases
    // ("Variable address not found" → segfault). Defer like SettingsController section open.
    const std::string action_message = action.message;
    const std::optional<std::string> action_payload = action.payload;
    const bool close_working_set = working_set_.ShouldCloseForAction(action_payload);
    BrowserThread::PostTask(BrowserThreadId::UI, [this, action_message, action_payload, close_working_set]() {
      if (close_working_set) {
        working_set_.Clear();
      }
      HandleLocalAction(action_message, action_payload);
    });
    return;
  }

  log().warning << "Chat action entry not found: " << entry_id;
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
      const std::string active_thread = thread_id.empty() ? ActiveThreadId() : thread_id;
      auto messages = chat_ports_.get_messages_page(active_thread, std::nullopt, 10000);
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", parsed.error) +
                                  R"( selectable="text"><p class="error">)" + StructuredTextParser::EscapeText(parsed.error) + "</p></div>";
            (void)chat_ports_.update_message(message);
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
      widgets_.Initialize(entry_id, parsed.widget_inits);
    }

    std::vector<TranscriptChatAction> chat_actions;
    chat_actions.reserve(parsed.chat_actions.size());
    for (const ParsedChatAction& action : parsed.chat_actions) {
      chat_actions.push_back({action.label, action.message, action.payload});
    }

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, entry_id);
    hydrated = HydrateChatActionButtons(hydrated, chat_actions);

    const std::string assistant_open =
        ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", raw_output) + R"( selectable="text">)";

    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? ActiveThreadId() : thread_id;
      auto messages = chat_ports_.get_messages_page(active_thread, std::nullopt, 10000);
      bool updated = false;
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = assistant_open + hydrated + "</div>";
            message.chat_actions = chat_actions;
            (void)chat_ports_.update_message(message);
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
        ai_message.content_rml = assistant_open + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.timestamp = util::NowUnixMs();
        ai_message.transport = MessageTransport::Local;
        (void)chat_ports_.append_message(ai_message);
      }
      (void)chat_ports_.update_preview(active_thread, parsed.rml);
    }

    working_set_.ApplyFromParse(entry_id, parsed.working_set_candidates);

    if (shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull) {
      const std::string active_thread =
          thread_id.empty() ? ActiveThreadId() : thread_id;
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
  ShellSetActivity(false);
  DirtyChat();
  DirtyShell();
}

void ChatController::HandleAgentEvent(const AgentEvent& event) {
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      chat_.status = "";
      ShellSetActivity(false);
    } else {
      ShellSetActivity(true);
      if (messaging_ready_) {
        SyncDisplayFromThread();
        DirtyChatTurns();
      }
    }
    DirtyChatChrome();
    break;
  case AgentEventType::ToolActivity:
    chat_.status = Rml::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    ShellSetActivity(true, chat_.status);
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
    if (AgentReady() && agent_ports_.has_conversation_entries && agent_ports_.has_conversation_entries() &&
        agent_ports_.last_conversation_entry_id && agent_ports_.complete_assistant_message &&
        agent_ports_.set_assistant_display_plain) {
      if (auto entry_id = agent_ports_.last_conversation_entry_id()) {
        agent_ports_.complete_assistant_message(*entry_id, event.message);
        agent_ports_.set_assistant_display_plain(*entry_id, event.message);
      }
      SyncDisplayFromThread();
      DirtyChatTurns();
    }
    chat_.loading = false;
    chat_.status = "";
    ShellSetActivity(false);
    DirtyChatChrome();
    break;
  }
}

void ChatController::WithSecrets(std::function<void()> action) {
  if (!unlock_gate_) {
    if (action) {
      action();
    }
    return;
  }
  if (unlock_gate_->IsUnlockInProgress()) {
    ShowToast(Tr("startup.still_preparing"));
    ShellDirty();
  }
  unlock_gate_->EnsureUnlocked(
      [this, action = std::move(action)](const bool unlocked) {
        if (!unlocked) {
          if (!unlock_gate_->IsUnlockInProgress()) {
            ShowToast("PIN required to continue");
            ShellDirty();
          }
          return;
        }
        if (!messaging_ready_) {
          WireMessagingBindings();
        }
        if (action) {
          action();
        }
      });
}

void ChatController::RefreshLlmSetupBanner() {
  const AppConfig& config = Store().Snapshot().config;
  use_llm_ = !config.llm.base_url.empty();
  constexpr const char* kRegisterBrief =
      "Register your identity in Me → Profile to use Brief assistant (or switch to Cloud/Ollama).";

  if (!use_llm_) {
    UserFeedback::NeedsSetup("Using mock replies — LLM is not configured.");
    return;
  }
  if (ResolvePreset(config) == "brief") {
    std::string brief_key;
    if (MessagingReady()) {
      if (auto identity = chat_ports_.get_identity()) {
        brief_key = identity->brief_llm_api_key;
      }
    }
    if (brief_key.empty()) {
      UserFeedback::NeedsSetup(kRegisterBrief);
      return;
    }
    if (ChromeSnapshot().banner_message == kRegisterBrief) {
      if (shell_feedback_.dismiss_banner) {
        shell_feedback_.dismiss_banner();
      }
    }
    return;
  }
  if (config.llm.require_api_key && config.llm.api_key.empty()) {
    UserFeedback::NeedsSetup("Add your API key in Me → Assistant to enable the assistant.");
  }
}

void ChatController::WireMessagingBindings() {
  if (!MessagingInitialized() || !AgentReady()) {
    return;
  }
  // Identity / Brief key / push registration are only valid after vault unlock.
  if (!MessagingReady()) {
    return;
  }
  messaging_ready_ = true;
  if (IThreadStore* store = chat_ports_.thread_store ? chat_ports_.thread_store() : nullptr) {
    if (agent_ports_.set_thread_store) {
      agent_ports_.set_thread_store(store);
    }
  }
  if (chat_ports_.bind_agent && agent_ports_.with_session) {
    agent_ports_.with_session([&](AgentSession& agent) { chat_ports_.bind_agent(agent); });
  }
  chat_.compose_disabled = false;
  DirtyChatChrome();
  chat_ports_.set_on_messages_changed([this]() {
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
  });
  chat_ports_.set_on_delivery_notice([this](const std::string& message) {
    ShowToast(message);
    ShellDirty();
  });
  chat_ports_.set_on_background_unread(
      [](std::string title, std::string body, std::string thread_id) {
        if (!ChatController::Instance().Store().Snapshot().profile_prefs.show_notifications) {
          return;
        }
        ILocalNotifier::Instance().NotifyIncoming(title, body, thread_id);
      });
  ILocalNotifier::Instance().SetActivationHandler([](std::string thread_id) {
    DesktopWindowChrome::RaiseAndFocus();
    if (!thread_id.empty()) {
      ChatController::Instance().OnSelectThread(thread_id);
    }
  });
  BackgroundSyncScheduler::Instance().SetSyncHandler([this](bool force) {
    if (!MessagingReady()) {
      return;
    }
    const bool call_wake = BackgroundSyncScheduler::Instance().ConsumeCallWake();
    // SyncInboxFromWake only queues IO; ring UI refreshes via OnRingChanged after ingest.
    if (call_wake && call_) {
      call_->OnCallWake();
    }
    chat_ports_.sync_inbox_from_wake(force);
  });
  IPushDeviceRegistrar::SetTokenChangedHandler([this](const std::string& /*token*/) {
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      if (!MessagingReady()) {
        return;
      }
      if (chat_ports_.sync_push_devices) {
        (void)chat_ports_.sync_push_devices(Store().Snapshot().profile_prefs.show_notifications);
      }
    });
  });
  if (chat_ports_.sync_push_devices) {
    (void)chat_ports_.sync_push_devices(Store().Snapshot().profile_prefs.show_notifications);
  }
  chat_ports_.set_on_thread_changed([this]() {
    RefreshFromMessaging();
    ContactsController::Instance().Refresh();
  });
  if (chat_ports_.has_router && chat_ports_.has_router()) {
    chat_ports_.set_on_local_action(
        [this](const std::string& message, const std::optional<std::string>& payload) {
          HandleLocalAction(message, payload);
        });
    chat_ports_.set_shared_ai_confirm_callback(
        [this](const std::string& thread_id, const AtAiMode mode, const std::string& prompt,
               std::function<void(bool confirmed, bool dont_ask_again)> done) {
          const char* mode_label = mode == AtAiMode::SharedFull ? "share the prompt and reply" : "share the AI reply";
          ShowConfirmWithCheckbox("Share with peer?",
              std::string("This will ") + mode_label +
                  " on the encrypted thread. Your local @ai assist stays private.",
              "Don't ask again for this conversation", false,
              [this, thread_id, done = std::move(done)](const bool ok, const bool dont_ask) {
                if (ok && dont_ask) {
                  chat_ports_.mark_shared_ai_confirmed(thread_id);
                }
                done(ok, dont_ask);
              });
        });
  }
  chat_ports_.set_on_action_message([this](const std::string& message) {
    ShowToast(message);
    ShellDirty();
  });
  RefreshFromMessaging();
  chat_ports_.tail_sync_active_e2e_thread();

  const bool auto_renew = Store().Snapshot().profile_prefs.auto_renew_registration;
  auto renew = chat_ports_.maybe_auto_renew_registration
      ? chat_ports_.maybe_auto_renew_registration(auto_renew)
      : Roe<bool>(false);
  if (!renew) {
    log().warning << "Auto-renew registration failed: " << renew.error().message;
  } else if (*renew) {
    log().info << "Network registration auto-renewed";
  } else {
    auto identity = chat_ports_.get_identity();
    if (identity && ShouldRenewRegistration(*identity) && !auto_renew) {
      UserFeedback::NeedsSetup("Network registration expires soon — renew in Me → Profile");
    }
  }
  // Always reload so Brief key from identity is applied after unlock (not only on renew).
  ReloadAgentConfig();
  RefreshLlmSetupBanner();
}

bool ChatController::Setup(Rml::Context* context) {
  StartupPhase setup_phase("ChatController::Setup");
  if (!context) {
    return false;
  }

  context_ = context;
  AppLifecycle::AddBackgroundListener([this]() { OnApplicationPause(); });
  AppLifecycle::AddForegroundListener([this]() {
    if (!messaging_ready_) {
      return;
    }
    const std::string active = ActiveThreadId();
    if (!active.empty() && MessagingReady()) {
      chat_ports_.warm_peer_for_thread(active);
    }
  });
  const AppConfig& config = Store().Snapshot().config;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  shell_.sessions = {{Rml::String("Chat"), Rml::String("Ask anything...")}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  StartupMark("chat_after_agent_ports");

  if (MessagingInitialized()) {
    WireMessagingBindings();
  }

  // Must go through Apply so Brief injects identity.brief_llm_api_key.
  Apply(ProjectAgent(config));
  log().info << "Chat initialized (model: " << config.llm.model << ")";

  DataModelHost::Instance().Clear();

  const auto register_enter_send = [this](Rml::Input::KeyIdentifier key) {
    if (!input_) {
      return;
    }
    input_->Register(KeyBinding{
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
        ctor.Bind("peer_link_status", &ChatController::Instance().chat_.peer_link_status);
        ctor.Bind("peer_link_banner", &ChatController::Instance().chat_.peer_link_banner);
        ctor.Bind("show_peer_link", &ChatController::Instance().chat_.show_peer_link);
        ctor.Bind("show_peer_link_banner", &ChatController::Instance().chat_.show_peer_link_banner);
        ctor.Bind("show_retry_peer_dial", &ChatController::Instance().chat_.show_retry_peer_dial);
        ctor.Bind("thread_encrypted", &ChatController::Instance().chat_.thread_encrypted);
        ctor.Bind("thread_is_ai", &ChatController::Instance().chat_.thread_is_ai);
        ctor.Bind("thread_is_private", &ChatController::Instance().chat_.thread_is_private);
        ctor.Bind("thread_is_public", &ChatController::Instance().chat_.thread_is_public);
        ctor.Bind("thread_is_group", &ChatController::Instance().chat_.thread_is_group);
        ctor.Bind("compose_disabled", &ChatController::Instance().chat_.compose_disabled);
        ctor.Bind("show_thread_actions", &ChatController::Instance().chat_.show_thread_actions);
        ctor.Bind("show_peer_sheet", &ChatController::Instance().chat_.show_peer_sheet);
        ctor.Bind("show_call_actions", &ChatController::Instance().chat_.show_call_actions);
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
        ctor.Bind("show_jump_to_latest", &ChatController::Instance().chat_.show_jump_to_latest);
        ctor.Bind("jump_to_latest_label", &ChatController::Instance().chat_.jump_to_latest_label);
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
        ctor.BindEventCallback("start_voice_call", &ChatController::StartVoiceCallCallback);
        ctor.BindEventCallback("start_video_call", &ChatController::StartVideoCallCallback);
        ctor.BindEventCallback("open_peer_sheet", &ChatController::OpenPeerSheetCallback);
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
        ctor.BindEventCallback("retry_peer_dial", &ChatController::RetryPeerDialCallback);
        ctor.BindEventCallback("messages_scroll", &ChatController::MessagesScrollCallback);
        ctor.BindEventCallback("jump_to_latest", &ChatController::JumpToLatestCallback);
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
          session_handle.RegisterMember("unread_display", &ChatController::SessionRow::unread_display);
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

  if (!ContactsController::Instance().RegisterModel(context)) {
    return false;
  }

  if (!PeoplePickerController::Instance().RegisterModel(context)) {
    return false;
  }

  if (shell_setup_.initialize) {
    shell_setup_.initialize(context);
  }
  // After Initialize clears state: Latin UI is ready; CJK waits on deferred faces.
  if (shell_setup_.fonts_ready) {
    shell_setup_.fonts_ready() = !UiLanguageNeedsCjkFonts();
  }

  shell_setup_.register_pane(
      {.key = "sidebar", .rml_path = "views/sidebar.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane(
      {.key = "contacts", .rml_path = "views/contacts.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane(
      {.key = "settings", .rml_path = "views/settings.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane(
      {.key = "home", .rml_path = "views/home.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane({.key = "chat",
                                       .rml_path = "views/chat.rml",
                                       .role = PaneRole::Primary,
                                       .provides_composer = true});
  shell_setup_.register_pane(
      {.key = "contact_detail", .rml_path = "views/contact_detail.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane(
      {.key = "settings_detail", .rml_path = "views/settings_detail.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane(
      {.key = "preview", .rml_path = "views/preview.rml", .role = PaneRole::Auxiliary, .toolbar_label = "Preview"});

  if (DocumentLoader::LoadFile(context, IAssetLocator::Instance().Resolve("samples/window_shell.rml")) == nullptr) {
    return false;
  }
  StartupMark("chat_after_window_shell");

  if (shell_setup_.update) {
    shell_setup_.update(context);
  }
  {
    StartupPhase phase("ShellHost::SyncLayout");
    if (shell_setup_.sync_layout) {
      shell_setup_.sync_layout();
    }
  }

  // Vault unlock + deferred fonts run after first present (DeferredStartup).
  if (messaging_ui_.snapshot) {
    chat_.compose_disabled = !messaging_ui_.snapshot().messaging_ready;
  } else {
    chat_.compose_disabled = !MessagingReady();
  }
  DirtyChatChrome();

  if (messaging_ready_) {
    OnHomeTabActivated();
  }

  // Brief key lives in the vault — refresh after unlock via WireMessagingBindings.
  // Non-Brief setup can be checked immediately (config-only).
  if (ResolvePreset(config) != "brief") {
    RefreshLlmSetupBanner();
  } else if (messaging_ready_) {
    RefreshLlmSetupBanner();
  }

  return true;
}

void ChatController::OnMessagingReady() {
  WireMessagingBindings();
  if (ChromeSnapshot().nav_tab == NavTab::Home) {
    OnHomeTabActivated();
  }
}

ChatController::AgentConfig ChatController::ProjectAgent(const AppConfig& config) {
  return {.llm = config.llm,
          .llm_api_key_env = config.llm_api_key_env,
          .promoted_mcp = config.promoted_mcp,
          .mcp_servers = config.mcp_servers,
          .search = config.search,
          .context = config.context};
}

void ChatController::Apply(const AgentConfig& config) {
  AgentConfig runtime = config;
  use_llm_ = !runtime.llm.base_url.empty();
  if (!AgentReady()) {
    return;
  }

  AppConfig preset_probe;
  preset_probe.llm = runtime.llm;
  if (ResolvePreset(preset_probe) == "brief") {
    runtime.llm.require_api_key = true;
    std::string brief_key;
    if (MessagingInitialized() && MessagingReady()) {
      if (auto identity = chat_ports_.get_identity()) {
        brief_key = identity->brief_llm_api_key;
      }
    }
    if (!brief_key.empty()) {
      runtime.llm.api_key = brief_key;
    } else {
      AppConfig last_probe;
      last_probe.llm = last_agent_runtime_.llm;
      if (ResolvePreset(last_probe) == "brief" && !last_agent_runtime_.llm.api_key.empty()) {
        // Me tab ReloadFromDisk may re-notify while identity is briefly unreadable.
        runtime.llm.api_key = last_agent_runtime_.llm.api_key;
      }
    }
  }

  if (!AgentConfigured() || runtime != last_agent_runtime_) {
    if (agent_ports_.set_tool_registration_hook) {
      agent_ports_.set_tool_registration_hook([this](ToolRegistry& tools) {
        if (MessagingInitialized() && chat_ports_.register_messaging_tools) {
          chat_ports_.register_messaging_tools(tools);
        }
      });
    }
    AppConfig configure = Store().IsInitialized()
                              ? Store().Snapshot().config
                              : AppConfig{};
    configure.llm = runtime.llm;
    configure.llm_api_key_env = runtime.llm_api_key_env;
    configure.promoted_mcp = runtime.promoted_mcp;
    configure.mcp_servers = runtime.mcp_servers;
    configure.search = runtime.search;
    configure.context = runtime.context;
    if (agent_ports_.configure) {
      agent_ports_.configure(configure);
    }
    last_agent_runtime_ = std::move(runtime);
  }
}

void ChatController::ReloadAgentConfig() {
  Apply(ProjectAgent(Store().Snapshot().config));
}

void ChatController::OnApplicationPause() {
  if (AgentReady() && agent_ports_.cancel) {
    agent_ports_.cancel();
  }
  if (messaging_ready_) {
    chat_ports_.suspend_libp2p_cold_peers();
  }
}

void ChatController::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_) {
    if (MessagingReady()) {
      BackgroundSyncScheduler::Instance().Tick();
      const auto now = std::chrono::steady_clock::now();
      if (chrome_.MaybePollPeerLink(now)) {
        DirtyChatHeader();
      }
    }
  }

  if (AgentReady() && agent_ports_.poll_events) {
    std::vector<AgentEvent> events;
    agent_ports_.poll_events(events);
    for (const AgentEvent& event : events) {
      HandleAgentEvent(event);
    }
  }
}

void ChatController::AfterLayout() {
  scroller_.ApplyPolicy();
}

void ChatController::Shutdown() {
  AppLifecycle::ClearBackgroundListeners();
  AppLifecycle::ClearForegroundListeners();
  IPushDeviceRegistrar::SetTokenChangedHandler(nullptr);
  // MessagingReady / ReachabilityUpdated are owned by Application.
  if (MessagingInitialized()) {
    if (chat_ports_.set_on_messages_changed) {
      chat_ports_.set_on_messages_changed(nullptr);
    }
    if (chat_ports_.set_on_delivery_notice) {
      chat_ports_.set_on_delivery_notice(nullptr);
    }
    if (chat_ports_.set_on_background_unread) {
      chat_ports_.set_on_background_unread(nullptr);
    }
    if (chat_ports_.set_on_thread_changed) {
      chat_ports_.set_on_thread_changed(nullptr);
    }
    if (chat_ports_.set_on_action_message) {
      chat_ports_.set_on_action_message(nullptr);
    }
    if (chat_ports_.set_on_local_action) {
      chat_ports_.set_on_local_action(nullptr);
    }
    if (chat_ports_.set_shared_ai_confirm_callback) {
      chat_ports_.set_shared_ai_confirm_callback(nullptr);
    }
  }
  if (AgentReady()) {
    StartupPhase phase("Shutdown::AgentSession");
    if (agent_ports_.cancel) {
      agent_ports_.cancel();
    }
    // ConfigureOnIO may still be running (and used to touch MessagingHub via the
    // tool hook). Wait before Application tears the hub down.
    if (agent_ports_.wait_for_configure_idle) {
      agent_ports_.wait_for_configure_idle();
    }
  }
  // Hub + ProfileSecrets lifetime is owned by Application::ShutdownMessaging.
  messaging_ready_ = false;
  pending_reply_.reset();
  context_ = nullptr;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  use_llm_ = false;
}

bool SetupChatController(Rml::Context* context) {
  return ChatController::Instance().Setup(context);
}

void UpdateChatController() {
  ChatController::Instance().Update();
}

void AfterLayoutChatController() {
  ChatController::Instance().AfterLayout();
}

void ShutdownChatController() {
  ChatController::Instance().Shutdown();
}

} // namespace pbr
