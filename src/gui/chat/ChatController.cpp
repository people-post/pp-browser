#include <stdexcept>
#include <filesystem>
#include <fstream>
#include "gui/chat/ChatController.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ChatAnswer.h"
#include "gui/chat/SessionListText.h"
#include "gui/chat/SuggestionPayload.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/BadgeAggregator.h"
#include "gui/BadgeNotifyPorts.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/ui/DesktopWindowChrome.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/IPushDeviceRegistrar.h"
#include "foundation/platform/NativeFileDialog.h"
#include "domain/messaging/AttachmentCache.h"
#include "foundation/platform/PlatformOpenFile.h"
#include "foundation/platform/PlatformOpenUrl.h"

#include "domain/ai/MarkdownToRml.h"
#include "domain/ai/StructuredTextParser.h"
#include "domain/ai/WorkingSetPolicy.h"
#include "domain/ai/conversation/Conversation.h"
#include "foundation/platform/IAssetLocator.h"
#include "domain/ui/ContextMenuHost.h"
#include "domain/ui/InputCoordinator.h"
#include "domain/ui/ChatFormHelper.h"
#include "domain/ui/RmlVariantHelpers.h"
#include "domain/ui/CalendarHelper.h"
#include "gui/chat/ChatWidgetStateBuilder.h"
#include "common/ui/WorkingSetCodec.h"
#include "common/Utilities.h"
#include "common/StartupTiming.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "domain/messaging/GroupTypes.h"
#include "domain/people/PeerDisplayLabel.h"
#include "domain/people/ContactJson.h"
#include "feature/registration/RegistrationClient.h"
#include "domain/messaging/AtAiParser.h"
#include "domain/messaging/CallThreadPresenceLogic.h"
#include "domain/messaging/ChatPayloadCodec.h"
#include "domain/messaging/ChatPayloadValidator.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/MessagingJson.h"
#include "domain/messaging/ReactionTypes.h"
#include "domain/messaging/SendRelayOptions.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/EmojiKey.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/DocumentLoader.h"
#include "gui/shell/DeferredStartup.h"
#include "gui/CallActionsPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/shell/ShellHost.h"
#include "gui/SettingsController.h"
#include "domain/ui/PaymentFeedback.h"
#include "gui/UserFeedback.h"
#include "gui/BlobQuotaRecoveryFlow.h"
#include "domain/mesh/reachability/Reachability.h"
#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/data/SessionStore.h"
#include "foundation/error/AppError.h"
#include "domain/net/BriefGuestLlmClient.h"
#include "foundation/platform/DeploymentProfile.h"
#include "domain/ui/ContextMenuHost.h"
#include "gui/contacts/ContactsController.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"

#include <ui/base/SystemInterface.h>
#include "gui/SettingsController.h"

#include "common/ValueJson.h"

#include <ui/dom/Context.h>
#include <ui/dom/SelectionController.h>
#include <ui/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/widgets/ElementFormControlTextArea.h>
#include <ui/base/StringUtilities.h>
#include <ui/base/SystemInterface.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string ToolActivityLabel(const std::string& tool_name, const std::string& status) {
  // brief_AI status events: tool = backend tool name, status = phase ("answer" has no tool).
  if (status == "answer") {
    return Tr("chat.status.writing");
  }
  if (tool_name == "search_web") {
    return Tr("chat.status.searching_web");
  }
  if (tool_name == "read_url") {
    return Tr("chat.status.reading_page");
  }
  if (tool_name == "search_opensearch") {
    return Tr("chat.status.searching_nfsc");
  }
  if (tool_name == "get_quote") {
    return Tr("chat.status.getting_quote");
  }
  if (tool_name == "web_search") {
    if (status == "running") {
      return Tr("chat.tool.searching_web");
    }
    if (status == "done") {
      return Tr("chat.tool.search_complete");
    }
    if (status == "error") {
      return Tr("chat.tool.search_failed");
    }
  }
  if (status == "running") {
    return Tr("chat.tool.running", {{"tool", tool_name}});
  }
  if (status == "done") {
    return Tr("chat.tool.finished", {{"tool", tool_name}});
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
              { "label": "Message", "message": "Start chat with Alice", "payload": "{\"type\":\"start_conversation\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"account\",\"value\":\"account:alice123\",\"primary\":true},{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":false}]}}" },
              { "label": "Add contact", "message": "Add Alice", "payload": "{\"type\":\"add_contact\",\"directory_hit\":{\"hit_id\":\"hit_alice\",\"display_name\":\"Alice Example\",\"nickname\":\"alice\",\"ids\":[{\"kind\":\"account\",\"value\":\"account:alice123\",\"primary\":true},{\"kind\":\"relay_user\",\"value\":\"relay:alice123\",\"primary\":false}]}}" }
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

ChatController* ChatController::installed_instance_ = nullptr;

void ChatController::InstallInstance(ChatController& controller) {
  installed_instance_ = &controller;
}

void ChatController::ClearInstance() {
  installed_instance_ = nullptr;
}

ChatController& ChatController::Instance() {
  if (!installed_instance_) {
    throw std::runtime_error("ChatController not installed");
  }
  return *installed_instance_;
}

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
                 .peer_link_direct = chat_.peer_link_direct,
                 .peer_link_via_hop = chat_.peer_link_via_hop,
                 .peer_link_via_relay = chat_.peer_link_via_relay,
                 .peer_link_connecting = chat_.peer_link_connecting,
                 .peer_link_degraded = chat_.peer_link_degraded,
                 .peer_link_failed = chat_.peer_link_failed,
                 .peer_link_ready = chat_.peer_link_ready,
                 .show_retry_peer_dial = chat_.show_retry_peer_dial,
                 .thread_encrypted = chat_.thread_encrypted,
                 .thread_is_ai = chat_.thread_is_ai,
                 .thread_is_private = chat_.thread_is_private,
                 .thread_is_public = chat_.thread_is_public,
                 .thread_is_group = chat_.thread_is_group,
                 .compose_disabled = chat_.compose_disabled,
                 .show_attach_button = chat_.show_attach_button,
                 .show_thread_actions = chat_.show_thread_actions,
                 .show_peer_sheet = chat_.show_peer_sheet,
                 .show_call_actions = chat_.show_call_actions,
                 .show_forget_memory = chat_.show_forget_memory,
                 .show_sync_with_peer = chat_.show_sync_with_peer,
                 .show_thread_menu = chat_.show_thread_menu,
                 .show_gap_banner = chat_.show_gap_banner,
                 .show_compromised_banner = chat_.show_compromised_banner,
                 .show_locked_out_banner = chat_.show_locked_out_banner,
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
             messaging_ready_,
             mesh_ready_) {
  redirectLogger("ChatController");
  scroller_.SetDirtyTurns([]() { DirtyChatTurns(); });
  working_set_.SetWidgetLookup([this](const std::string& entry_id) -> const TurnWidgetState* {
    return widgets_.Find(entry_id);
  });
  chrome_.SetAiAttachInfo([this]() {
    ChatThreadChrome::AiAttachInfo info;
    info.brief_preset = ResolvePreset(Store().Snapshot().config) == "brief";
    info.ai_usable = messaging_ready_ && facade_ && AgentCloudReady();
    info.home = ChromeSnapshot().nav_tab == NavTab::Home;
    return info;
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

void ChatController::BindConversationsFacade(ConversationsFacade* facade) {
  facade_ = facade;
  scroller_.BindConversationsFacade(facade);
  chrome_.BindConversationsFacade(facade);
  if (facade) {
    facade->SetDisplayRowDecorator([this](std::vector<MessageDisplayRow>& rows) { DecorateAiImageRows(rows); });
  }
}

void ChatController::BindRegisterMessagingTools(std::function<void(ToolRegistry&)> hook) {
  register_messaging_tools_ = std::move(hook);
}

void ChatController::BindAgentPorts(AgentUiPorts ports) {
  agent_ports_ = std::move(ports);
}

void ChatController::BindContactsNotify(ContactsNotifyPorts ports) {
  contacts_notify_ = std::move(ports);
}

void ChatController::BindPeoplePickerNotify(PeoplePickerNotifyPorts ports) {
  people_picker_notify_ = std::move(ports);
}

void ChatController::BindEmojiPickerNotify(EmojiPickerNotifyPorts ports) {
  emoji_picker_notify_ = std::move(ports);
}

void ChatController::InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus) {
  const std::string key = NormalizeEmojiKey(emoji);
  if (key.empty() || chat_.compose_disabled) {
    return;
  }
  const std::string next = std::string(chat_.draft.c_str()) + key;
  chat_.draft = next.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::Element* el = context_->GetDocument(0)->GetElementById("draft-input");
  auto* draft = ui_dynamic_cast<ui::ElementFormControlTextArea*>(el);
  if (!draft) {
    return;
  }
  draft->SetValue(next.c_str());
  const ui::String value = draft->GetValue();
  const int end = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(value.size()));
  if (!restore_composer_focus) {
    // Keyboard-panel multi-insert: caret only (unfocused SetSelectionRange does not OSK).
    draft->SetSelectionRange(end, end);
    return;
  }
  // Popover close remounts via SyncLayout; place caret after remount too.
  focus_draft_after_sync_ = true;
  draft->Focus();
  draft->SetSelectionRange(end, end);
}

void ChatController::ReactWithEmoji(const std::string& message_id, const std::string& emoji) {
  ToggleReaction(message_id, emoji);
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

bool ChatController::AgentCloudReady() const {
  if (!AgentReady() || !Store().IsInitialized()) {
    return false;
  }
  const AppConfig& config = Store().Snapshot().config;
  // Mock replies (no base_url) are always usable once the agent session exists.
  if (config.llm.base_url.empty()) {
    return true;
  }
  if (agent_ports_.snapshot) {
    if (!agent_ports_.snapshot().cloud_ready) {
      return false;
    }
  } else if (!AgentConfigured()) {
    return false;
  }
  if (ResolvePreset(config) == "brief") {
    std::string registered;
    std::string guest;
    if (MessagingReady() && facade_) {
      if (auto identity = facade_->GetIdentity()) {
        registered = identity->brief_llm_api_key;
        guest = identity->brief_llm_guest_api_key;
      }
    }
    std::string brief_key = ResolveBriefLlmApiKey(registered, guest);
    if (brief_key.empty()) {
      AppConfig last_probe;
      last_probe.llm = last_agent_runtime_.llm;
      if (ResolvePreset(last_probe) == "brief") {
        brief_key = last_agent_runtime_.llm.api_key;
      }
    }
    return !brief_key.empty();
  }
  if (config.llm.require_api_key) {
    return !config.llm.api_key.empty() || !last_agent_runtime_.llm.api_key.empty();
  }
  return true;
}

void ChatController::BindShellSetup(ShellSetupPorts ports) {
  shell_setup_ = std::move(ports);
}

bool ChatController::MessagingInitialized() const {
  if (facade_) {
    return facade_->Snapshot().initialized;
  }
  if (messaging_ui_.snapshot) {
    return messaging_ui_.snapshot().initialized;
  }
  return false;
}

bool ChatController::MessagingReady() const {
  if (facade_) {
    return facade_->Snapshot().messaging_ready;
  }
  if (messaging_ui_.snapshot) {
    return messaging_ui_.snapshot().messaging_ready;
  }
  return false;
}

const std::string& ChatController::ActiveThreadId() const {
  static const std::string kEmpty;
  if (facade_) {
    return facade_->ActiveThreadId();
  }
  return kEmpty;
}

void ChatController::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
}

void ChatController::BindBadgeNotify(BadgeNotifyPorts ports) {
  badge_notify_ = std::move(ports);
}

void ChatController::BindInputCoordinator(InputCoordinator& input) {
  input_ = &input;
}

void ChatController::BindCallActions(CallActionsPorts ports) {
  call_actions_ = std::move(ports);
}

void ChatController::BindUnlockEnsure(UnlockEnsurePorts ports) {
  unlock_ensure_ = std::move(ports);
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

void ChatController::BindSurfaceNotify(ChatSurfaceNotifyPorts ports) {
  surface_notify_ = std::move(ports);
  if (surface_notify_.push_surface) {
    chrome_.SetNotifySurfaceChanged([this]() { NotifySurfaceChanged(); });
  } else {
    chrome_.SetNotifySurfaceChanged({});
  }
}

void ChatController::BindMessagingUi(MessagingUiPorts ports) {
  messaging_ui_ = std::move(ports);
}

ShellChromeSnapshot ChatController::ChromeSnapshot() const {
  return shell_navigation_.snapshot ? shell_navigation_.snapshot() : ShellChromeSnapshot{};
}

ChatSurfaceSnapshot ChatController::BuildSurfaceSnapshot() const {
  ChatSurfaceSnapshot snap;
  snap.has_active_thread = !ActiveThreadId().empty();
  if (badge_notify_.sessions_unread) {
    snap.sessions_unread = badge_notify_.sessions_unread();
  }
  return snap;
}

void ChatController::NotifySurfaceChanged() {
  if (surface_notify_.push_surface) {
    surface_notify_.push_surface(BuildSurfaceSnapshot());
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

void ChatController::ShellSetActivity(const bool visible, const ui::String& message) {
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
    shell_feedback_.show_confirm(title, message, std::move(on_result), {});
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


void ChatController::OpenWorkingSetCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  const std::optional<int> block_index = EventArgAsInt(args, 1);
  if (!block_index || *block_index < 0) {
    return;
  }
  Instance().working_set_.Open(std::string(args[0].Get<ui::String>().c_str()), *block_index);
}

void ChatController::SendMessageCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                   const ui::VariantList& /*args*/) {
  Instance().OnSendMessage();
}

void ChatController::SendSuggestionCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().SendUserText(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::SendSuggestionActionCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                  const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  const std::string id(args[0].Get<ui::String>().c_str());
  auto payload = SuggestionPayload(id, LocalizationService::Instance().ResolvedLanguage());
  if (!payload) {
    return;
  }
  // The bubble shows the localized sentence; the payload makes the app run the function itself.
  Instance().SendUserText(Tr("home.suggestion." + id + "_prompt"), std::move(payload));
}

void ChatController::SubmitFormCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                  const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING || args[1].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().SubmitForm(std::string(args[0].Get<ui::String>().c_str()),
                        std::string(args[1].Get<ui::String>().c_str()));
}

void ChatController::SendChatActionCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING) {
    return;
  }

  const std::optional<int> action_index = EventArgAsInt(args, 1);
  if (!action_index || *action_index < 0) {
    return;
  }

  Instance().SendChatAction(std::string(args[0].Get<ui::String>().c_str()), *action_index);
}

void ChatController::OpenChatLinkCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  const std::optional<int> link_index = EventArgAsInt(args, 1);
  if (!link_index) {
    return;
  }
  Instance().OpenChatLink(std::string(args[0].Get<ui::String>().c_str()), *link_index);
}

void ChatController::StopTurnCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& /*args*/) {
  Instance().OnStopTurn();
}

void ChatController::ToggleReactionCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING || args[1].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().ToggleReaction(std::string(args[0].Get<ui::String>().c_str()),
                            std::string(args[1].Get<ui::String>().c_str()));
}

void ChatController::OpenEmojiInsertCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                             const ui::VariantList& /*args*/) {
  Instance().OpenEmojiInsertMenu(&ev);
}

void ChatController::AttachFileCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& /*args*/) {
  Instance().OnAttachFile();
}

void ChatController::RemoveImageCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                         const ui::VariantList& /*args*/) {
  Instance().OnRemoveImage();
}

void ChatController::OpenAttachmentCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OpenAttachment(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::RetryAttachmentCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().RetryAttachmentDownload(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::DownloadAttachmentCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().DownloadAttachment(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::CalendarPrevCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                    const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().CalendarPrev(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::CalendarNextCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                    const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().CalendarNext(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::SelectCalendarDayCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                         const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING || args[1].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().SelectCalendarDay(std::string(args[0].Get<ui::String>().c_str()),
                               std::string(args[1].Get<ui::String>().c_str()));
}

void ChatController::NewChatCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/, const ui::VariantList& /*args*/) {
  Instance().OnNewChat();
}

void ChatController::NewMessageCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                        const ui::VariantList& /*args*/) {
  Instance().OnNewMessage();
}

void ChatController::OpenNewSessionMenuCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                                const ui::VariantList& /*args*/) {
  Instance().OnOpenNewSessionMenu(ev);
}

void ChatController::OpenThreadActionsMenuCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                                   const ui::VariantList& /*args*/) {
  Instance().OnOpenThreadActionsMenu(ev);
}

void ChatController::StartCallCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                       const ui::VariantList& /*args*/) {
  Instance().OnStartCall(ev);
}

void ChatController::OpenPeerSheetCallback(ui::DataModelHandle /*model*/, ui::Event& ev,
                                           const ui::VariantList& /*args*/) {
  Instance().OnOpenPeerSheet(ev);
}

void ChatController::SelectThreadCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/, const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnSelectThread(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::CloseThreadCallback(ui::DataModelHandle /*model*/, ui::Event& ev, const ui::VariantList& args) {
  ev.StopPropagation();
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().OnCloseThread(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::ClearHistoryCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  Instance().OnClearHistory();
}

void ChatController::ForgetMemoryCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  Instance().OnForgetMemory();
}

void ChatController::SyncWithPeerCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
  Instance().OnSyncWithPeer();
}

void ChatController::RetryGapSyncCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                           const ui::VariantList& /*args*/) {
  Instance().OnRetryGapSync();
}

void ChatController::StartNewSecureChatCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                                const ui::VariantList& /*args*/) {
  Instance().OnStartNewSecureChat();
}

void ChatController::PauseIntegrityCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnPauseIntegrityOnly();
}

void ChatController::CopyPskKeyCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                      const ui::VariantList& /*args*/) {
  Instance().OnCopyPskKey();
}

void ChatController::TogglePskImportCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                           const ui::VariantList& /*args*/) {
  Instance().OnTogglePskImport();
}

void ChatController::ImportPskCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                     const ui::VariantList& /*args*/) {
  Instance().OnImportPsk();
}

void ChatController::VerifyPskCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                       const ui::VariantList& /*args*/) {
  Instance().OnVerifyPsk();
}

void ChatController::RotatePskExportCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& /*args*/) {
  Instance().OnRotatePskExport();
}

void ChatController::FinalizeThreadDisplay() {
  working_set_.ClearAll();
  RefreshFromMessaging();
  RestoreWorkingSetsFromActiveThread();
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
  facade_->ClearActiveThread();
  working_set_.ClearAll();
  ShellSetPrimaryPane("home");
  RefreshFromMessaging();
  ShellSyncLayout();
  NotifySurfaceChanged();
}

void ChatController::OnSessionsTabActivated() {
  working_set_.ClearAll();
}

void ChatController::OnSelectThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  if (facade_->OpenThread(thread_id)) {
    ILocalNotifier::Instance().ClearForThread(thread_id);
    facade_->MaybeTailSync(thread_id);
    ShellSetPrimaryPane("chat");
    FinalizeThreadDisplay();
  }
}

void ChatController::OnCloseThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }

  auto finish_close = [this, thread_id]() {
    if (!facade_->CloseThread(thread_id)) {
      UserFeedback::Fail(Tr("chat.error.delete_failed"));
      NotifySurfaceChanged();
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
    NotifySurfaceChanged();
  };

  auto dismiss_and_close = [this, finish_close](const std::string& group_id) {
    (void)facade_->DismissLocalGroup(group_id);
    finish_close();
  };

  auto thread = facade_->GetThread(thread_id);
  if (thread && *thread && (*thread)->kind == ThreadKind::Group && (*thread)->group_id) {
    const std::string group_id = *(*thread)->group_id;
    std::string local_identity;
    if (auto identity = facade_->GetIdentity()) {
      local_identity = identity->account_id;
    }

    bool is_owner = false;
    if (auto owner = facade_->IsLocalOwner(group_id)) {
      is_owner = *owner;
    } else if (auto roster = facade_->ListGroupRoster(group_id)) {
      for (const GroupRosterMember& member : *roster) {
        if (member.member_identity == local_identity && member.role == MemberRole::Owner) {
          is_owner = true;
          break;
        }
      }
    }

    std::vector<GroupRosterMember> members;
    if (auto roster = facade_->ListGroupRoster(group_id)) {
      members = *roster;
    }

    std::vector<GroupRosterMember> successors;
    for (const GroupRosterMember& member : members) {
      if (member.member_identity == local_identity) {
        continue;
      }
      if (facade_->IsMemberUnreachable(group_id, member.member_identity)) {
        continue;
      }
      successors.push_back(member);
    }

    bool owner_on_roster = false;
    if (auto owner_id = facade_->OwnerIdentity(group_id)) {
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
                                     if (auto left = facade_->LeaveGroup(group_id); !left) {
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
            if (auto contact = facade_->FindContactByIdentity(member.member_identity,
                                                                                  ContactIdKind::Account)) {
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
                Tr("chat.group.transfer_to", {{"name", label}}),
                nullptr,
                [this, finish_close, group_id, successor]() {
                  if (auto left = facade_->LeaveAsOwner(group_id, successor); !left) {
                    UserFeedback::Fail(left.error().message);
                    NotifySurfaceChanged();
                    return;
                  }
                  finish_close();
                },
                "../icons/contacts.svg",
            });
          }
          actions.push_back({
              "dismiss_local",
              Tr("chat.group.dismiss_local"),
              nullptr,
              [dismiss_and_close, group_id]() { dismiss_and_close(group_id); },
              "../icons/trash.svg",
              true,
          });
          ContextMenuHost::Instance().ShowActions(ui::Vector2i(120, 120), std::move(actions));
          NotifySurfaceChanged();
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

  auto thread = facade_->GetActiveThread();
  const bool is_ai = thread && thread->kind == ThreadKind::Ai;
  std::string message = Tr("chat.clear_history.body");
  if (is_ai) {
    message += " " + Tr("chat.clear_history.body_ai");
  } else if (thread && thread->kind == ThreadKind::Direct && thread->encrypted) {
    message += " " + Tr("chat.clear_history.body_secure");
  }

  if (is_ai) {
    ShowConfirmWithCheckbox(Tr("chat.clear_history"), message, Tr("chat.clear_history.forget_checkbox"), false,
        [this, thread_id](bool ok, bool forget_memory) {
          if (!ok) {
            return;
          }
          if (!facade_->ClearThreadHistory(thread_id, forget_memory)) {
            return;
          }
          chat_.draft = "";
          chat_.status = "";
          chat_.loading = false;
          pending_reply_.reset();
          widgets_.ClearAll();
          RefreshFromMessaging();
          NotifySurfaceChanged();
        });
  } else {
    ShowConfirm(Tr("chat.clear_history"), message,
                               [this, thread_id](bool ok) {
                                 if (!ok) {
                                   return;
                                 }
                                 if (!facade_->ClearThreadHistory(thread_id, false)) {
                                   return;
                                 }
                                 chat_.draft = "";
                                 chat_.status = "";
                                 chat_.loading = false;
                                 pending_reply_.reset();
                                 widgets_.ClearAll();
                                 RefreshFromMessaging();
                                 NotifySurfaceChanged();
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

  ShowConfirm(Tr("chat.forget_memory.title"),
      Tr("chat.forget_memory.body"),
      [this, thread_id](bool ok) {
        if (!ok) {
          return;
        }
        if (!facade_->ForgetThreadMemory(thread_id)) {
          return;
        }
        NotifySurfaceChanged();
      });
}

void ChatController::LoadOlderHistoryCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                              const ui::VariantList& /*args*/) {
  Instance().OnLoadOlderHistory();
}

void ChatController::RetryPeerDialCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                           const ui::VariantList& /*args*/) {
  Instance().OnRetryPeerDial();
}

void ChatController::SendSharedAssistantRelay(const std::string& thread_id, const AtAiMode mode,
                                              const std::string& plain_text) {
  if (!messaging_ready_ || plain_text.empty()) {
    return;
  }
  auto thread = facade_->GetThread(thread_id);
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
  (void)facade_->SendUserMessage(thread_id, plain_text, opts);
}

void ChatController::RefreshFromMessaging() {
  const int prev_sessions = ChromeSnapshot().nav_badges.sessions_unread;
  const int prev_contacts = ChromeSnapshot().nav_badges.contacts_unread;
  SyncShellSessions();
  SyncDisplayFromThread();
  chrome_.Update();
  SyncComposerInputState();
  if (badge_notify_.refresh) {
    badge_notify_.refresh();
  }
  // Inbox ingest (incl. call_invite) completes on IO; reconcile ring after messages change.
  if (call_actions_.refresh_pending_ring) {
    call_actions_.refresh_pending_ring();
  }
  DirtyChat();
  DirtyShell();
  NotifySurfaceChanged();
  const NavBadgeState& badges = ChromeSnapshot().nav_badges;
  if (badges.sessions_unread != prev_sessions || badges.contacts_unread != prev_contacts) {
    ShellRemountNavRail();
  }
}

void ChatController::OnProfileDataReset() {
  messaging_ready_ = false;
  mesh_ready_ = false;
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
  auto threads = facade_->ListThreads();
  if (!threads) {
    return;
  }
  std::vector<Thread> sorted_threads = *threads;
  std::sort(sorted_threads.begin(), sorted_threads.end(),
            [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });

  const std::string active_id = ActiveThreadId();
  const int64_t now_ms = util::NowUnixMs();
  for (const Thread& thread : sorted_threads) {
    if (IsCallControlShadowThread(thread, sorted_threads)) {
      continue;
    }
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = facade_
                    ? facade_->ResolveThreadLabel(thread).title.c_str()
                    : thread.title.c_str();
    // The stored default AI title is English; show it in the UI language.
    if (thread.kind == ThreadKind::Ai && row.title == "New chat") {
      row.title = Tr("chat.new_chat").c_str();
    }
    row.preview = thread.preview.c_str();
    row.kind = SessionVisualKind(thread);
    row.unread_count = thread.unread_count;
    row.unread_display = FormatBadgeCount(thread.unread_count).c_str();
    row.date_label = SessionDateLabel(thread.updated_at, now_ms).c_str();
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

void ChatController::MessagesScrollCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnMessagesScroll();
}

void ChatController::JumpToLatestCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                          const ui::VariantList& /*args*/) {
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

void ChatController::OnLockPublicToThisDevice() {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  ShowConfirm(Tr("chat.device_lock.confirm_title"), Tr("chat.device_lock.confirm_body"),
              [this, thread_id](bool ok) {
                if (!ok) {
                  return;
                }
                WithSecrets([this, thread_id]() {
                  auto locked = facade_->LockPublicThreadToThisDevice(thread_id);
                  if (!locked) {
                    ShowToast(locked.error().message);
                  }
                  RefreshFromMessaging();
                });
              });
}

void ChatController::UpdateThreadChrome() {
  chrome_.Update();
  SyncComposerInputState();
}

void ChatController::SyncComposerInputState() {
  chat_.composer_input_disabled = chat_.compose_disabled || chat_.attachment_uploading || chat_.image_preparing;
}

void ChatController::OnAttachFile() {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  if (chat_.composer_input_disabled || chat_.attachment_uploading || !chat_.show_attach_button) {
    return;
  }
  if (InAiComposerContext()) {
    OnAttachAiImage();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  ShowOpenFileDialog(Backend::GetWindow(), [this, thread_id](std::vector<std::string> paths) {
    AppRuntime::PostUI([this, thread_id, paths = std::move(paths)]() mutable {
      if (paths.empty()) {
        return;
      }
      StartAttachmentUpload(std::move(paths.front()));
    });
  });
}

void ChatController::StartAttachmentUpload(const std::string& path) {
  if (!facade_ || chat_.attachment_uploading) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  chat_.attachment_uploading = true;
  chat_.attachment_draft_name = std::filesystem::path(path).filename().string().c_str();
  chat_.status = Tr("chat.attachment.uploading").c_str();
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();

  BlobQuotaRecoveryFlow::RunUploadAsync<ThreadMessage>(
      [this, thread_id, path](std::function<void(Roe<ThreadMessage>)> done) {
        facade_->SendAttachmentFromPathAsync(thread_id, path, std::move(done));
      },
      [this](Roe<ThreadMessage> sent) {
        chat_.attachment_uploading = false;
        chat_.attachment_draft_name = "";
        chat_.status = "";
        SyncComposerInputState();
        DirtyChatChrome();
        DirtyChatHeader();
        if (!sent) {
          UserFeedback::Fail(UserFeedback::UserMessage(sent.error()));
          return;
        }
        SyncDisplayFromThread();
        scroller_.RequestScrollToLatest();
        UpdateSidebarPreview(sent->text);
      },
      [this]() { return facade_->PlanRelayQuotaRecovery(); },
      [this]() { return facade_->FreeOldestRelayBlobSlot(); });
}

bool ChatController::InAiComposerContext() const {
  if (!facade_) {
    return false;
  }
  if (auto thread = facade_->GetActiveThread()) {
    return thread->kind == ThreadKind::Ai;
  }
  return ChromeSnapshot().nav_tab == NavTab::Home;
}

void ChatController::OnAttachAiImage() {
  ShowOpenImageFileDialog(
      Backend::GetWindow(),
      [this](std::vector<std::string> paths) {
        AppRuntime::PostUI([this, paths = std::move(paths)]() mutable {
          if (paths.empty()) {
            return;
          }
          StartAiImagePrepare(std::move(paths.front()));
        });
      },
      /*include_heic=*/true);
}

namespace {

/** Result of preparing an image on a worker: the bytes for the request plus a thumbnail file for the UI. */
struct PreparedDraft {
  BriefAiImage image;
  std::string file_path;
  int width = 0;
  int height = 0;
};

/** Pseudo thread whose blobs_view holds AI image thumbnails; wiped with the other session plaintext views. */
constexpr const char* kAiImageViewThread = "ai-images";

Roe<PreparedDraft> PrepareAiImageDraft(const std::string& path, const std::string& profile_dir) {
  auto prepared = PrepareAiImageFromFile(path);
  if (!prepared) {
    return prepared.error();
  }
  PreparedDraft draft;
  draft.image.mime = prepared->mime;
  draft.image.data = prepared->bytes;
  draft.width = prepared->width;
  draft.height = prepared->height;
  if (!profile_dir.empty()) {
    const std::filesystem::path dir = AttachmentViewRoot(profile_dir, kAiImageViewThread);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / (util::GenerateUuid() + (prepared->mime == "image/png" ? ".png" : ".jpg"));
    if (!ec) {
      std::ofstream out(file, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char*>(prepared->bytes.data()), static_cast<std::streamsize>(prepared->bytes.size()));
      out.close();
      if (out) {
        draft.file_path = file.string();
      } else {
        std::filesystem::remove(file, ec);
      }
    }
  }
  return draft;
}

void RemoveSessionFile(const std::string& path) {
  if (!path.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
}

} // namespace

void ChatController::StartAiImagePrepare(std::string path) {
  DiscardPendingAiImage();
  const uint64_t generation = ++image_prepare_generation_;
  std::string name = std::filesystem::path(path).filename().string();
  chat_.image_chip = true;
  chat_.image_preparing = true;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = name.c_str();
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();

  const std::string profile_dir = facade_ ? facade_->ProfileDataDir() : std::string();
  // Decoding and re-encoding a photo is blocking work: off the UI thread, reply on it.
  AppRuntime::PostWorkerNormal([this, path = std::move(path), profile_dir, generation, name = std::move(name)]() mutable {
    Roe<PreparedDraft> result = PrepareAiImageDraft(path, profile_dir);
    AppRuntime::PostUI([this, generation, name = std::move(name), result = std::move(result)]() mutable {
      if (generation != image_prepare_generation_) {
        if (result) {
          RemoveSessionFile(result->file_path); // removed or replaced while it was being prepared
        }
        return;
      }
      chat_.image_preparing = false;
      if (!result) {
        // Sizes and kinds only in the log: no file name, no path.
        log().warning << "AI image not prepared: kind=" << result.error().code;
        chat_.image_chip = false;
        chat_.image_draft_name = "";
        SyncComposerInputState();
        DirtyChatChrome();
        DirtyChatHeader();
        UserFeedback::Fail(Tr(AiImagePrepErrorKey(AiImagePrepErrorOf(result.error()))));
        return;
      }
      log().info << "AI image ready: " << result->image.data.size() << " bytes";
      pending_image_ = PendingAiImage{
          .image = std::move(result->image), .name = std::move(name),
          .file_path = std::move(result->file_path),
          .width = result->width,
          .height = result->height};
      chat_.image_thumb_ready = !pending_image_->file_path.empty();
      chat_.image_thumb_src = pending_image_->file_path.c_str();
      SyncComposerInputState();
      DirtyChatChrome();
      DirtyChatHeader();
    });
  });
}

void ChatController::OnRemoveImage() {
  DiscardPendingAiImage();
}

void ChatController::DiscardPendingAiImage() {
  ++image_prepare_generation_;
  if (pending_image_) {
    RemoveSessionFile(pending_image_->file_path);
    pending_image_.reset();
  }
  if (!chat_.image_chip && !chat_.image_preparing) {
    return;
  }
  chat_.image_chip = false;
  chat_.image_preparing = false;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = "";
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

bool ChatController::SendImageQuestion(const std::string& text) {
  if (!messaging_ready_ || !facade_ || !pending_image_ || !agent_ports_.submit_image_to_thread) {
    return false;
  }
  if (!AgentCloudReady()) {
    RefreshLlmSetupBanner();
    return false;
  }
  // Taken out first: opening the AI thread from Home switches threads, which discards a pending image.
  PendingAiImage image = std::move(*pending_image_);
  pending_image_.reset();
  if (ChromeSnapshot().nav_tab == NavTab::Home && !EnsureHomeOutboundSession()) {
    pending_image_ = std::move(image);
    return false;
  }
  chat_.image_chip = false;
  chat_.image_thumb_ready = false;
  chat_.image_thumb_src = "";
  chat_.image_draft_name = "";

  const std::string thread_id = ActiveThreadId();
  const std::string question = text.empty() ? Tr("chat.image.default_question") : text;
  const std::string message_id = util::GenerateUuid();
  if (!image.file_path.empty()) {
    ai_image_files_[message_id] = AiImageView{image.file_path, image.width, image.height};
  }
  widgets_.ExpireOpenForms();
  DirtyChatTurns();
  chat_.loading = true;
  chat_.status = "";
  (void)facade_->NameAiThreadFromFirstMessage(thread_id, AiThreadTitleFromMessage(question));
  UpdateSidebarPreview(question);
  DirtyChatChrome();
  DirtyChatHeader();
  log().info << "Submitting an image question to the agent session";
  agent_ports_.submit_image_to_thread(thread_id, question, AgentImageTurn{.image = std::move(image.image), .message_id = message_id});
  return true;
}

void ChatController::DecorateAiImageRows(std::vector<MessageDisplayRow>& rows) const {
  // Only the user's own questions in an AI thread carry the marker; a peer's or the AI's text that
  // happens to start with it is left alone.
  if (!facade_) {
    return;
  }
  auto thread = facade_->GetActiveThread();
  if (!thread || thread->kind != ThreadKind::Ai) {
    return;
  }
  for (MessageDisplayRow& row : rows) {
    if (row.row_class != "message-row-user") {
      continue;
    }
    std::string rml = row.content_rml.c_str();
    if (rml.find(kAiImageTurnMarker) == std::string::npos) {
      continue;
    }
    // The thumbnail is a session file: after a restart (or a vault lock) only the marker remains.
    std::string src;
    int width = 0;
    int height = 0;
    if (const auto view = ai_image_files_.find(row.message_id.c_str()); view != ai_image_files_.end()) {
      std::error_code ec;
      if (std::filesystem::is_regular_file(view->second.file_path, ec)) {
        src = StructuredTextParser::EscapeText(view->second.file_path);
        width = view->second.width;
        height = view->second.height;
      }
    }
    row.content_rml = DecorateAiImageBubble(std::move(rml), src, width, height, Tr("chat.image.marker")).c_str();
  }
}

void ChatController::OpenAttachment(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  auto path = facade_->AttachmentLocalPathForMessage(thread_id, message_id);
  if (!path) {
    UserFeedback::Fail(Tr("chat.attachment.not_ready"));
    return;
  }
  const auto open_file = [path = *path]() {
    if (!PlatformOpenFile(path)) {
      UserFeedback::Fail(Tr("chat.attachment.open_failed"));
    }
  };
  if (facade_->AttachmentOpenNeedsConfirmForMessage(thread_id, message_id)) {
    ShowConfirm(Tr("chat.attachment.open_confirm_title"), Tr("chat.attachment.open_confirm_body"),
                [open_file](const bool ok) {
                  if (ok) {
                    open_file();
                  }
                });
    return;
  }
  open_file();
}

void ChatController::RetryAttachmentDownload(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  facade_->RetryAttachmentDownload(ActiveThreadId(), message_id);
}

void ChatController::DownloadAttachment(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  facade_->RequestAttachmentDownload(ActiveThreadId(), message_id);
}

void ChatController::UpdatePeerLinkChrome() {
  chrome_.UpdatePeerLink();
  DirtyChatHeader();
}

void ChatController::ResetChatPanelState() {
  chat_.attachment_uploading = false;
  chat_.attachment_draft_name = "";
  chrome_.ResetPanelState();
  SyncComposerInputState();
}

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  if (!facade_ || !facade_->GetActiveThread()) {
    chrome_.ResetPanelState();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (facade_) {
    facade_->EnsureThreadAttachments(thread_id);
  }
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id);
  if (thread_changed) {
    DiscardPendingAiImage(); // the chip belongs to the composer of the thread it was picked in
  }

  const std::string prev_tail_id =
      chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
  const size_t prev_count = chat_.messages.size();

  chat_.messages = facade_
      ? facade_->BuildDisplayRows(thread_id, scroller_.LoadedMinDisplayOrder(),
                                  scroller_.LoadedMaxDisplayOrder())
      : std::vector<MessageDisplayRow>{};
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;

  scroller_.EndDisplaySync(thread_changed, prev_tail_id, prev_count);
}

// The delta's entry_id is the pending turn's user message id; the final AssistantReady carries the
// persisted assistant message id, so the live bubble is a synthetic last row that ClearStreamingRow
// drops right before FinishAssistantReply rebuilds the rows.
void ChatController::OnAssistantDelta(const AgentEvent& event) {
  if (!messaging_ready_) {
    return;
  }
  if (!streaming_ || streaming_->row_id != "streaming-" + event.entry_id) {
    ClearStreamingRow();
    streaming_ = StreamingRow{};
    streaming_->row_id = "streaming-" + event.entry_id;
    streaming_->thread_id = event.thread_id;
  }
  streaming_->text = event.text;
  streaming_->dirty = true;
  FlushStreamingRow();
}

void ChatController::FlushStreamingRow() {
  if (!streaming_) {
    return;
  }
  bool changed = false;
  const auto now = std::chrono::steady_clock::now();
  if (streaming_->dirty && ShouldRenderStreamDelta(streaming_->last_render, now)) {
    ChatAnswerRml answer = BuildMarkdownAnswer(streaming_->text);
    streaming_->rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", streaming_->text) +
                      R"( selectable="text">)" + InjectEntryPlaceholders(answer.rml, streaming_->row_id) + "</div>";
    chat_links_[streaming_->row_id] = std::move(answer.links);
    streaming_->last_render = now;
    streaming_->dirty = false;
    changed = true;
  }
  if (streaming_->rml.empty() || streaming_->thread_id != ActiveThreadId()) {
    return;
  }
  // Row rebuilds (scroller paging, thread sync) drop the synthetic row; put it back.
  if (chat_.messages.empty() || std::string(chat_.messages.back().message_id.c_str()) != streaming_->row_id) {
    MessageDisplayRow row;
    row.message_id = streaming_->row_id.c_str();
    row.row_class = "message-row-ai";
    chat_.messages.push_back(std::move(row));
    changed = true;
  }
  if (changed) {
    chat_.messages.back().content_rml = streaming_->rml.c_str();
    DirtyChatTurns();
  }
}

void ChatController::ClearStreamingRow() {
  if (!streaming_) {
    return;
  }
  chat_links_.erase(streaming_->row_id);
  streaming_.reset();
}

void ChatController::OnStopTurn() {
  if (AgentReady() && agent_ports_.cancel) {
    agent_ports_.cancel();
  }
}

void ChatController::OpenChatLink(const std::string& entry_id, const int link_index) {
  auto links = chat_links_.find(entry_id);
  if (links == chat_links_.end() && messaging_ready_ && facade_ && entry_id.rfind("streaming-", 0) != 0) {
    // After a restart the map is empty: rebuild from the message's own stored text.
    if (auto messages = facade_->GetMessagesPage(ActiveThreadId(), std::nullopt, 10000)) {
      for (const ThreadMessage& message : *messages) {
        if (message.id == entry_id && message.sender_contact_id == kAiAssistantContactId) {
          links = chat_links_.emplace(entry_id, RecoverChatLinks(message.text)).first;
          break;
        }
      }
    }
  }
  if (links == chat_links_.end()) {
    return;
  }
  const std::optional<std::string> url = ResolveChatLink(links->second, link_index);
  if (!url) {
    return;
  }
  ConfirmAndOpenUrl(*url);
}

void ChatController::ConfirmAndOpenUrl(const std::string& url) {
  if (!IsHttpsUrl(url)) {
    return;
  }
  if (skip_link_confirm_this_run_) {
    (void)PlatformOpenUrl(url);
    return;
  }
  // The opt-out lives in memory only: the next launch asks again.
  ShowConfirmWithCheckbox(Tr("chat.open_link_title", {{"host", UrlHost(url)}}), Tr("chat.open_link_body", {{"url", url}}),
                          Tr("chat.open_link_skip"), false, [this, url](const bool ok, const bool skip) {
                            if (!ok) {
                              return;
                            }
                            skip_link_confirm_this_run_ = skip;
                            (void)PlatformOpenUrl(url);
                          });
}

void ChatController::RestoreWorkingSetsFromActiveThread() {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  auto messages = facade_->GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!messages) {
    return;
  }
  for (const ThreadMessage& message : *messages) {
    if (!message.working_set_json || message.working_set_json->empty()) {
      continue;
    }
    auto candidates = WorkingSetCandidatesFromJson(*message.working_set_json);
    if (candidates.empty()) {
      continue;
    }
    (void)working_set_.RestoreEntry(message.id, candidates, message.chat_actions);
  }
}

void ChatController::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    auto action_json = TryParseObject(*payload);
    auto action_type = action_json ? action_json->getString("type") : std::nullopt;
    if (action_type && *action_type == "tool_permission") {
      const std::string approval_id = action_json->getString("approval_id").value_or("");
      const std::string decision = action_json->getString("decision").value_or("");
      if (!agent_ports_.resume_tool_permission) {
        ShowToast(Tr("chat.permission.not_ready"));
        return;
      }
      auto resumed = agent_ports_.resume_tool_permission(approval_id, decision, message);
      if (!resumed) {
        ShowToast(resumed.error().message);
      }
      return;
    }
    if (action_type && *action_type == "open_url") {
      // Same guard as open_chat_link: https only, and the user confirms the host first.
      ConfirmAndOpenUrl(action_json->getString("url").value_or(""));
      return;
    }
    if (!action_type && action_json && !action_json->getString("tool").value_or("").empty() && chat_.thread_is_ai) {
      // A tool payload (e.g. "load more" of the article feed) is run by the agent, not by the contact
      // dispatcher. AI threads only: elsewhere the router would hand it straight back here.
      SendUserText(message, payload);
      return;
    }
    if (action_type && *action_type == "fork_group") {
      const std::string confirmed_payload = *payload;
      ShowConfirm(Tr("chat.fork_group.title"),
          Tr("chat.fork_group.body"),
          [this, message, confirmed_payload](bool ok) {
            if (!ok) {
              return;
            }
            auto result = facade_->DispatchAction(confirmed_payload);
            if (!result) {
              log().warning << "Local action failed: " << result.error().message;
              ShowToast(result.error().message);
              NotifySurfaceChanged();
              return;
            }
            if (*result) {
              SendUserText(message, *result);
              return;
            }
            RefreshFromMessaging();
            if (contacts_notify_.refresh) {
              contacts_notify_.refresh();
            }
            if (!ActiveThreadId().empty()) {
              ShellSelectNavTab(NavTab::Sessions);
              ShellSetPrimaryPane("chat");
              if (ChromeSnapshot().layout_mode == LayoutMode::Compact) {
                ShellOpenCompactChat();
              }
            }
            NotifySurfaceChanged();
          });
      return;
    }
    auto result = facade_->DispatchAction(*payload);
    if (!result) {
      // Never re-route the same action payload through MessageRouter — that loops
      // HandleLocalAction → SendUserText → Route → HandleLocalAction until stack overflow.
      log().warning << "Local action failed: " << result.error().message;
      ShowToast(result.error().message);
      NotifySurfaceChanged();
      return;
    }
    if (*result) {
      SendUserText(message, *result);
      return;
    }
    RefreshFromMessaging();
    if (contacts_notify_.refresh) {
      contacts_notify_.refresh();
    }
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
  // Enter only checks focus: while an image is still being prepared the question must not go out
  // alone (the image would then attach itself to the next one).
  if (chat_.loading || chat_.compose_disabled || chat_.image_preparing) {
    return;
  }

  const std::string text = util::Trim(chat_.draft.c_str());
  const bool with_image = pending_image_.has_value() && InAiComposerContext();
  if (text.empty() && !with_image) {
    return;
  }

  // On a phone the on-screen keyboard stays up for as long as the composer has focus; sending is what closes it.
  const auto close_keyboard = [this] {
    if (!Platform::IsMobile() || !context_ || context_->GetNumDocuments() == 0) {
      return;
    }
    if (ui::Element* draft = context_->GetDocument(0)->GetElementById("draft-input")) {
      draft->Blur();
    }
  };

  if (!text.empty()) {
    if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
      ShowToast(Tr("chat.error.message_too_long"));
      return;
    }
  }

  if (with_image) {
    // The draft is cleared only once the question is really on its way; a refused send keeps the text and the chip.
    if (SendImageQuestion(text)) {
      chat_.draft = "";
      DirtyChatChrome();
      scroller_.RequestScrollToLatest();
      close_keyboard();
    }
    return;
  }
  chat_.draft = "";
  DirtyChatChrome();
  scroller_.RequestScrollToLatest();
  close_keyboard();
  SendUserText(text);
}

void ChatController::OnNewChat() {
  if (!messaging_ready_) {
    return;
  }

  (void)facade_->CreateNewAiThread();
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
  NotifySurfaceChanged();
}

void ChatController::OnNewMessage() {
  if (people_picker_notify_.open_free) {
    people_picker_notify_.open_free();
  }
}

namespace {

const char* kReactionPresets[] = {"👍", "❤️", "😂", "😮", "😢", "🙏"};

std::string FindMessageIdFromElement(ui::Element* element) {
  for (ui::Element* cur = element; cur; cur = cur->GetParentNode()) {
    if (cur->HasAttribute("message-id")) {
      const ui::String value = cur->GetAttribute("message-id", ui::String());
      return std::string(value.c_str());
    }
  }
  return {};
}

} // namespace

void ChatController::ToggleReaction(const std::string& message_id, const std::string& emoji) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  const std::string key = NormalizeEmojiKey(emoji);
  if (key.empty()) {
    return;
  }

  // If local already has this emoji active on the target, clear; else add.
  bool mine_active = false;
  auto page = facade_->GetMessagesPage(thread_id, std::nullopt, 500);
  if (page) {
    struct Slot {
      int64_t order = -1;
      bool active = false;
    };
    Slot latest;
    for (const ThreadMessage& message : *page) {
      if (message.content_type != ChatContentType::Annotation) {
        continue;
      }
      if (message.sender_contact_id != kLocalSelfContactId) {
        continue;
      }
      if (!message.target_message_id || *message.target_message_id != message_id) {
        continue;
      }
      auto fields = ChatPayloadCodec::DecodeAnnotationJson(message.payload_json);
      std::string annotation_type = kAnnotationTypeReaction;
      std::string value = message.text;
      if (fields) {
        annotation_type = fields->annotation_type;
        value = fields->value.empty() ? message.text : fields->value;
      }
      if (!IsReactionAnnotationType(annotation_type)) {
        continue;
      }
      if (NormalizeEmojiKey(value) != key) {
        continue;
      }
      if (message.display_order >= latest.order) {
        latest.order = message.display_order;
        latest.active = annotation_type == kAnnotationTypeReaction;
      }
    }
    mine_active = latest.active;
  }

  auto sent = mine_active ? facade_->ClearReaction(thread_id, message_id, key)
                          : facade_->SendReaction(thread_id, message_id, key);
  if (!sent) {
    if (shell_feedback_.show_toast) {
      shell_feedback_.show_toast(sent.error().message, ToastDuration::Short);
    }
    return;
  }
  SyncDisplayFromThread();
  NotifySurfaceChanged();
}

void ChatController::ShowReactionMorePrompt(const std::string& message_id) {
  if (emoji_picker_notify_.open_react) {
    emoji_picker_notify_.open_react(message_id);
    return;
  }
  if (!shell_feedback_.show_prompt) {
    return;
  }
  shell_feedback_.show_prompt(
      Tr("chat.react.title"), Tr("chat.react.prompt"), "",
      [this, message_id](bool confirmed, std::string value) {
        if (!confirmed) {
          return;
        }
        ToggleReaction(message_id, value);
      });
}

std::string ChatController::MessagePlainText(const std::string& message_id) const {
  if (!facade_ || message_id.empty()) {
    return {};
  }
  const auto page = facade_->GetMessagesPage(ActiveThreadId(), std::nullopt, 500);
  if (!page) {
    return {};
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id) {
      continue;
    }
    // Local-pipeline AI answers are stored as UI block documents; copy what the user reads.
    if (const auto prose = StructuredTextParser::PlainTextIfBlocks(message.text)) {
      return *prose;
    }
    return message.text;
  }
  return {};
}

void ChatController::ComposeWithQuote(const std::string& prefix, const std::string& text) {
  if (chat_.compose_disabled || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  // The quote goes under what the user is about to type; long messages are quoted by their beginning.
  constexpr size_t kMaxQuoteChars = 300;
  std::string quote = "> ";
  size_t chars = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    const auto byte = static_cast<unsigned char>(text[i]);
    if ((byte & 0xC0) != 0x80 && ++chars > kMaxQuoteChars) {
      quote += "\xE2\x80\xA6";
      break;
    }
    quote += text[i];
    if (text[i] == '\n') {
      quote += "> ";
    }
  }
  const std::string value = prefix + "\n\n" + quote;
  chat_.draft = value.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  auto* draft = ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById("draft-input"));
  if (!draft) {
    return;
  }
  draft->SetValue(value.c_str());
  draft->Focus();
  // Caret right after the prefix, above the quote.
  const int caret = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(draft->GetValue(), static_cast<int>(prefix.size()));
  draft->SetSelectionRange(caret, caret);
}

void ChatController::OpenEmojiInsertMenu(ui::Event* ev) {
  if (chat_.compose_disabled) {
    return;
  }
  if (emoji_picker_notify_.open_insert) {
    emoji_picker_notify_.open_insert();
    return;
  }
  // Fallback when picker is not wired (tests / headless): keep preset strip.
  const ui::Vector2i position = ev ? MenuPositionBelowEvent(*ev) : ui::Vector2i(120, 120);
  std::vector<ContextMenuAction> actions;
  for (const char* emoji : kReactionPresets) {
    actions.push_back({
        std::string("insert_") + emoji,
        emoji,
        nullptr,
        [this, emoji]() { InsertEmojiIntoDraft(emoji); },
    });
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenNewSessionMenu(ui::Event& ev) {
  const ui::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "chat_with_ai",
      Tr("chat.menu.chat_with_ai"),
      nullptr,
      [this]() { OnNewChat(); },
      "../icons/sparkle.svg",
  });
  actions.push_back({
      "message_contact",
      Tr("chat.menu.message_a_contact"),
      nullptr,
      [this]() { OnNewMessage(); },
      "../icons/contacts.svg",
  });
  actions.push_back({
      "find_someone",
      Tr("chat.menu.find_someone"),
      nullptr,
      [this]() { OnFindSomeone(); },
      "../icons/message.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenThreadActionsMenu(ui::Event& ev) {
  // Anchor near the right edge of the trigger so the menu stays in the header corner.
  const ui::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (chat_.thread_is_public && messaging_ready_ && facade_) {
    const std::string thread_id = ActiveThreadId();
    auto can_lock = facade_->CanLockPublicToThisDevice(thread_id);
    if (can_lock && *can_lock) {
      actions.push_back({
          "lock_public_device",
          Tr("chat.device_lock.menu"),
          nullptr,
          [this]() { OnLockPublicToThisDevice(); },
          "../icons/lock.svg",
      });
    }
  }
  if (chat_.show_sync_with_peer && !chat_.sync_in_progress) {
    actions.push_back({
        "sync_with_peer",
        Tr("chat.menu.sync_with_peer"),
        nullptr,
        [this]() { OnSyncWithPeer(); },
        "../icons/sync.svg",
    });
  }
  if (chat_.show_thread_actions) {
    actions.push_back({
        "clear_history",
        Tr("chat.menu.clear_history"),
        nullptr,
        [this]() { OnClearHistory(); },
        "../icons/trash.svg",
        true,
    });
  }
  if (chat_.show_forget_memory) {
    actions.push_back({
        "forget_memory",
        Tr("chat.menu.forget_ai_memory"),
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

void ChatController::OnStartCall(ui::Event& ev) {
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty() || !call_actions_.start_call) {
    return;
  }
  auto start_call = call_actions_.start_call;
  const bool video_available =
      !call_actions_.video_call_available || call_actions_.video_call_available();
  if (!video_available) {
    (void)start_call(thread_id, false);
    return;
  }

  const ui::Vector2i position = MenuPositionBelowRightAlignedEvent(ev);
  std::vector<ContextMenuAction> actions;
  actions.push_back({
      "call_voice",
      Tr("call.start.voice"),
      nullptr,
      [thread_id, start_call]() { (void)start_call(thread_id, false); },
      "../icons/phone.svg",
  });
  actions.push_back({
      "call_video",
      Tr("call.start.video"),
      nullptr,
      [thread_id, start_call]() { (void)start_call(thread_id, true); },
      "../icons/video.svg",
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnOpenPeerSheet(ui::Event& ev) {
  if (!messaging_ready_ || !chat_.show_peer_sheet) {
    return;
  }
  auto thread = facade_->GetActiveThread();
  if (!thread) {
    return;
  }

  const ui::Vector2i position = MenuPositionBelowEvent(ev);

  std::vector<ContextMenuAction> actions;
  if (thread->kind == ThreadKind::Direct) {
    const PeerDisplayLabel label = facade_->ResolveThreadLabel(*thread);
    const std::string peer_id = thread->peer_identity_value;
    std::optional<std::string> dm_contact_id = label.contact_id;
    if (!dm_contact_id && !thread->participant_contact_ids.empty()) {
      const std::string& candidate = thread->participant_contact_ids.front();
      if (!candidate.empty()) {
        if (auto contact = facade_->GetContact(candidate); contact && *contact) {
          dm_contact_id = candidate;
        }
      }
    }
    if (dm_contact_id) {
      const std::string contact_id = *dm_contact_id;
      actions.push_back({
          "add_people",
          Tr("chat.menu.add_people"),
          nullptr,
          [this, contact_id]() {
            if (people_picker_notify_.open_from_dm) {
              people_picker_notify_.open_from_dm(contact_id);
            }
          },
          "../icons/group.svg",
      });
      actions.push_back({
          "view_contact",
          Tr("chat.menu.view_contact"),
          nullptr,
          [this, contact_id]() {
            if (contacts_notify_.select_contact) {
              contacts_notify_.select_contact(contact_id);
            }
          },
          "../icons/contacts.svg",
      });
    } else if (!peer_id.empty()) {
      actions.push_back({
          "add_contact",
          Tr("chat.menu.add_to_contacts"),
          nullptr,
          [this, peer_id]() {
            DirectoryHit hit;
            if (auto shadow = facade_->GetDirectoryShadow(peer_id)) {
              hit = *shadow;
            } else {
              hit.hit_id = peer_id;
              if (peer_id.rfind("account:", 0) == 0) {
                hit.account_id = peer_id;
                hit.ids = {{ContactIdKind::Account, peer_id, true}};
              } else {
                hit.ids = {{ContactIdKind::RelayUser, peer_id, false}};
              }
            }
            auto created = facade_->AddContactFromDirectoryHit(hit);
            if (!created) {
              UserFeedback::Fail(Tr("chat.error.add_contact_failed"));
              NotifySurfaceChanged();
              return;
            }
            if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail(Tr("chat.error.directory_missing_account"));
                NotifySurfaceChanged();
                return;
              }
              facade_->RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::Account), *hit.account_id,
                                              *hit.signing_public_key_b64, "directory");
            }
            if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail(Tr("chat.error.directory_missing_account"));
                NotifySurfaceChanged();
                return;
              }
              facade_->RegisterPeerKemKey(ContactIdKindToString(ContactIdKind::Account), *hit.account_id,
                                          *hit.kem_public_key_b64, "directory");
            }
            facade_->RegisterContactDirectEndpoints(*created);
            // Bind stranger DM (empty participants) to the new contact id.
            ThreadChannel channel = ThreadChannel::E2ePublic;
            if (auto active = facade_->GetActiveThread();
                active && active->kind == ThreadKind::Direct) {
              channel = active->channel;
            }
            (void)facade_->FindOrCreateDirectThread(created->id, channel);
            if (contacts_notify_.select_contact) {
              contacts_notify_.select_contact(created->id);
            }
            facade_->NotifyThreadChanged();
          },
          "../icons/contacts.svg",
      });
    }
    if (!peer_id.empty()) {
      actions.push_back({
          "copy_id",
          Tr("contacts.copy_id"),
          nullptr,
          [this, peer_id]() {
            if (ui::SystemInterface* system = ui::GetSystemInterface()) {
              system->SetClipboardText(peer_id.c_str());
            }
            ShowToast(Tr("chat.toast.id_copied"));
            NotifySurfaceChanged();
          },
          "../icons/copy.svg",
      });
    }
  } else if (thread->kind == ThreadKind::Group && thread->group_id) {
    const std::string thread_id = thread->id;
    const std::string group_id = *thread->group_id;
    const std::string current_local = thread->local_title;
    const PeerDisplayLabel label = facade_->ResolveThreadLabel(*thread);
    const std::string shared_default = label.shared_title.value_or(thread->title);

    actions.push_back({
        "rename_for_me",
        Tr("chat.group.rename_for_me"),
        nullptr,
        [this, thread_id, current_local, shared_default]() {
          ShowPrompt(Tr("chat.group.rename_for_me"), Tr("chat.group.rename_for_me_prompt"),
              current_local.empty() ? shared_default : current_local,
              [this, thread_id](bool ok, std::string value) {
                if (!ok) {
                  return;
                }
                if (auto saved = facade_->SetThreadLocalTitle(thread_id, value); !saved) {
                  UserFeedback::Fail(saved.error().message);
                }
                NotifySurfaceChanged();
              });
        },
        "../icons/message.svg",
    });
    if (!current_local.empty()) {
      actions.push_back({
          "clear_my_name",
          Tr("chat.group.clear_my_name"),
          nullptr,
          [this, thread_id]() {
            (void)facade_->SetThreadLocalTitle(thread_id, "");
            NotifySurfaceChanged();
          },
          "../icons/trash.svg",
      });
    }

    bool is_owner = false;
    std::string local_identity;
    if (auto identity = facade_->GetIdentity()) {
      local_identity = identity->account_id;
      if (auto roster = facade_->ListGroupRoster(group_id)) {
        for (const auto& member : *roster) {
          if (member.member_identity == identity->account_id && member.role == MemberRole::Owner) {
            is_owner = true;
            break;
          }
        }
      }
    }
    const bool owner_unreachable = facade_->IsOwnerUnreachable(group_id);
    if (is_owner) {
      actions.push_back({
          "rename_for_everyone",
          Tr("chat.group.rename_for_everyone"),
          nullptr,
          [this, group_id, shared_default]() {
            ShowPrompt(Tr("chat.group.rename_for_everyone"), Tr("chat.group.rename_for_everyone_prompt"),
                shared_default, [this, group_id](bool ok, std::string value) {
                  if (!ok) {
                    return;
                  }
                  if (value.empty()) {
                    UserFeedback::Fail(Tr("chat.group.title_required"));
                    NotifySurfaceChanged();
                    return;
                  }
                  if (auto renamed = facade_->RenameGroupShared(group_id, value); !renamed) {
                    UserFeedback::Fail(renamed.error().message);
                  } else {
                    facade_->NotifyThreadChanged();
                  }
                  NotifySurfaceChanged();
                });
          },
          "../icons/group.svg",
      });
      for (const std::string& unreachable_id : facade_->ListUnreachableMembers(group_id)) {
        if (unreachable_id == local_identity) {
          continue;
        }
        std::string name = unreachable_id;
        if (auto contact =
                facade_->FindContactByIdentity(unreachable_id, ContactIdKind::Account)) {
          if (*contact) {
            name = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
            if (name.empty()) {
              name = unreachable_id;
            }
          }
        }
        actions.push_back({
            "remove_unreachable_" + unreachable_id,
            Tr("chat.group.remove_unreachable", {{"name", name}}),
            nullptr,
            [this, group_id, unreachable_id, name]() {
              ShowConfirm(Tr("chat.group.remove_member_title"),
                  Tr("chat.group.remove_member_confirm", {{"name", name}}),
                  [this, group_id, unreachable_id](bool ok) {
                    if (!ok) {
                      return;
                    }
                    if (auto removed =
                            facade_->RemoveMemberByIdentity(group_id, unreachable_id);
                        !removed) {
                      UserFeedback::Fail(removed.error().message);
                    } else {
                      facade_->NotifyThreadChanged();
                    }
                    NotifySurfaceChanged();
                  });
            },
            "../icons/trash.svg",
            true,
        });
      }
    } else if (owner_unreachable) {
      actions.push_back({
          "rename_for_everyone",
          Tr("chat.group.rename_for_everyone"),
          nullptr,
          [this]() {
            ShowToast(Tr("chat.group.owner_only"));
            NotifySurfaceChanged();
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
  ui::Element* el = context_->GetDocument(0)->GetElementById("draft-input");
  auto* draft = ui_dynamic_cast<ui::ElementFormControlTextArea*>(el);
  if (!draft) {
    return;
  }
  draft->Focus();
  const ui::String value = draft->GetValue();
  const int end = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(value.size()));
  draft->SetSelectionRange(end, end);
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
  (void)facade_->UpdatePreview(thread_id, preview_text);
  SyncShellSessions();
  DirtyShell();
}

bool ChatController::EnsureHomeOutboundSession() {
  if (!messaging_ready_) {
    return false;
  }
  if (!facade_->CreateNewAiThread()) {
    return false;
  }
  ShellSelectNavTab(NavTab::Sessions);
  ShellSetPrimaryPane("chat");
  FinalizeThreadDisplay();
  NotifySurfaceChanged();
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
  if (!expect_agent_work && messaging_ready_ && facade_ && facade_->HasRouter()) {
    expect_agent_work = facade_->ExpectsAgentWork(
        ActiveThreadId(), trimmed, user_payload);
  } else if (!expect_agent_work) {
    expect_agent_work = true;
  }

  // Peer relay send only needs messaging_ready; AI / Brief / mock turns need agent_cloud_ready.
  if (expect_agent_work && !AgentCloudReady()) {
    RefreshLlmSetupBanner();
    return;
  }

  if (expect_agent_work) {
    chat_.loading = true;
    chat_.status = "";
  }
  // A fresh AI thread is named after its first question, so the list is not all "New chat". Done before
  // the preview update, which rebuilds the session rows once.
  (void)facade_->NameAiThreadFromFirstMessage(ActiveThreadId(), AiThreadTitleFromMessage(trimmed));
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
    (void)facade_->AppendMessage(user_message);
    SyncDisplayFromThread();
    DirtyChatTurns();
    log().debug << "Using mock assistant response";
    pending_reply_ = PendingReply{.entry_id = user_message.id, .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  if (messaging_ready_ && facade_ && facade_->HasRouter()) {
    log().info << "Routing message via MessageRouter";
    auto routed = facade_->RouteMessage(ActiveThreadId(), trimmed, std::move(user_payload));
    if (!routed) {
      chat_.loading = false;
      UserFeedback::Fail(PaymentErrorUserMessage(routed.error().message));
      DirtyChatChrome();
      SyncDisplayFromThread();
      return;
    }
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

  std::optional<TranscriptChatAction> resolved = working_set_.LookupChatAction(entry_id, action_index);
  if (!resolved) {
    const std::string thread_id = ActiveThreadId();
    auto messages = facade_->GetMessagesPage(thread_id, std::nullopt, 10000);
    if (messages) {
      for (const ThreadMessage& message : *messages) {
        if (message.id != entry_id) {
          continue;
        }
        if (action_index >= static_cast<int>(message.chat_actions.size())) {
          log().warning << "Chat action index out of range: entry=" << entry_id << " index=" << action_index
                        << " size=" << message.chat_actions.size();
          return;
        }
        resolved = message.chat_actions[static_cast<size_t>(action_index)];
        break;
      }
    }
  }

  if (!resolved) {
    log().warning << "Chat action entry not found: " << entry_id << " thread=" << ActiveThreadId()
                  << " working_set_entry=" << working_set_.ActiveEntryId();
    return;
  }

  // Copy by value: layout-mutating actions close the working set / remount panes. Doing that
  // synchronously destroys the click target and leaves RmlUi data views with dangling aliases
  // ("Variable address not found" → segfault). Defer like SettingsController section open.
  const std::string action_message = resolved->message;
  const std::optional<std::string> action_payload = resolved->payload;
  const bool close_working_set = working_set_.ShouldCloseForAction(action_payload);
  AppRuntime::PostUI([this, action_message, action_payload, close_working_set]() {
    if (close_working_set) {
      working_set_.Clear();
    }
    HandleLocalAction(action_message, action_payload);
  });
}

void ChatController::FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, const bool from_llm,
                                    const std::string& finish_reason, const std::string& thread_id,
                                    ResponseGoal response_goal, RenderMode render_mode, const AtAiMode shared_ai_mode,
                                    const std::vector<BriefAiSource>& sources) {
  const bool markdown = render_mode == RenderMode::Markdown;
  if (!from_llm && !markdown) {
    response_goal = InferResponseGoalFromBlocksJson(raw_output);
  }

  ChatAnswerRml markdown_answer;
  std::string answer_text;
  ParseResult parsed;
  if (markdown) {
    // Cap the body first and append the link after: cutting the finished text at the store's limit
    // would drop exactly the trailing details link that the link table is rebuilt from.
    answer_text = WithDetailsLink(
        StructuredTextParser::StorableText(raw_output, kMaxComposeTextBytes - kDetailsLinkHeadroomBytes), sources,
        Tr("chat.view_details"));
    markdown_answer = BuildMarkdownAnswer(answer_text);
    parsed.ok = true;
    parsed.rml = markdown_answer.rml;
  } else {
    parsed = from_llm ? StructuredTextParser::ParseFromLlmOutput(raw_output, response_goal, render_mode)
                      : StructuredTextParser::ParseBlocksJson(raw_output, response_goal, render_mode);
  }
  if (!parsed.ok) {
    log().warning << "Failed to parse assistant reply: " << parsed.error;
    if (from_llm && !finish_reason.empty()) {
      log().warning << "LLM finish_reason: " << finish_reason;
    }
    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? ActiveThreadId() : thread_id;
      auto messages = facade_->GetMessagesPage(active_thread, std::nullopt, 10000);
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", parsed.error) +
                                  R"( selectable="text"><p class="error">)" + StructuredTextParser::EscapeText(parsed.error) + "</p></div>";
            (void)facade_->UpdateMessage(message);
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

    // Working-set / bubble buttons must key send_chat_action to the ThreadMessage that
    // actually stores chat_actions — not a stale agent entry id when we append a new row.
    std::string action_entry_id = entry_id;
    const std::string active_thread = thread_id.empty() ? ActiveThreadId() : thread_id;
    bool message_exists = false;
    if (messaging_ready_ && !action_entry_id.empty()) {
      auto messages = facade_->GetMessagesPage(active_thread, std::nullopt, 10000);
      if (messages) {
        for (const ThreadMessage& message : *messages) {
          if (message.id == action_entry_id) {
            message_exists = true;
            break;
          }
        }
      }
    }
    if (messaging_ready_ && !message_exists) {
      action_entry_id = util::GenerateUuid();
    }

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, action_entry_id);
    std::vector<WorkingSetCandidate> working_set_candidates =
        working_set_.HydrateCandidates(parsed.working_set_candidates, action_entry_id);
    // Panel owns long_list row actions; only inline chips for chat-only replies.
    if (working_set_candidates.empty()) {
      hydrated = HydrateChatActionButtons(hydrated, chat_actions);
    }

    std::optional<std::string> working_set_json;
    if (!working_set_candidates.empty()) {
      working_set_json = WorkingSetCandidatesToJson(working_set_candidates);
    }

    const std::string assistant_open =
        ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", raw_output) + R"( selectable="text">)";

    if (messaging_ready_) {
      if (message_exists) {
        auto messages = facade_->GetMessagesPage(active_thread, std::nullopt, 10000);
        if (messages) {
          for (ThreadMessage& message : *messages) {
            if (message.id == action_entry_id) {
              if (markdown) {
                // The session stored the raw answer; the details link must be in the stored text too,
                // or the link table cannot be rebuilt from it after a restart.
                message.text = StructuredTextParser::StorableText(answer_text);
              }
              message.content_rml = assistant_open + hydrated + "</div>";
              message.chat_actions = chat_actions;
              message.working_set_json = working_set_json;
              (void)facade_->UpdateMessage(message);
              break;
            }
          }
        }
      } else {
        ThreadMessage ai_message;
        ai_message.id = action_entry_id;
        ai_message.thread_id = active_thread;
        ai_message.sender_contact_id = kAiAssistantContactId;
        ai_message.text = StructuredTextParser::StorableText(markdown ? answer_text : raw_output);
        ai_message.content_rml = assistant_open + hydrated + "</div>";
        ai_message.chat_actions = chat_actions;
        ai_message.working_set_json = working_set_json;
        ai_message.timestamp = util::NowUnixMs();
        ai_message.transport = MessageTransport::Local;
        if (auto appended = facade_->AppendMessage(ai_message)) {
          if (!appended->id.empty()) {
            action_entry_id = appended->id;
            if (action_entry_id != ai_message.id) {
              hydrated = InjectEntryPlaceholders(parsed.rml, action_entry_id);
              working_set_candidates =
                  working_set_.HydrateCandidates(parsed.working_set_candidates, action_entry_id);
              if (working_set_candidates.empty()) {
                hydrated = HydrateChatActionButtons(hydrated, chat_actions);
              }
              if (!working_set_candidates.empty()) {
                working_set_json = WorkingSetCandidatesToJson(working_set_candidates);
              }
              ai_message.id = action_entry_id;
              ai_message.content_rml = assistant_open + hydrated + "</div>";
              ai_message.working_set_json = working_set_json;
              (void)facade_->UpdateMessage(ai_message);
            }
          }
        } else {
          log().warning << "Failed to append assistant message for chat actions: "
                        << appended.error().message;
        }
      }
      (void)facade_->UpdatePreview(active_thread, parsed.rml);
    }

    if (markdown) {
      chat_links_[action_entry_id] = std::move(markdown_answer.links);
    }

    working_set_.ApplyFromParse(action_entry_id, working_set_candidates, chat_actions);

    if (shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull) {
      // The peer gets prose, never the blocks document (fenced answers used to go out as raw JSON).
      SendSharedAssistantRelay(active_thread, shared_ai_mode, StructuredTextParser::PlainText(raw_output));
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
  // An image turn rejected by the server (413 / 400) is worded here, not with the server's text.
  const char* image_error_key = AiImageErrorKindKey(event.error_kind);
  const std::string error_message = image_error_key ? Tr(image_error_key) : event.message;
  switch (event.type) {
  case AgentEventType::LoadingChanged:
    chat_.loading = event.loading;
    if (!event.loading) {
      if (streaming_) { // turn ended without a persisted answer
        ClearStreamingRow();
        SyncDisplayFromThread();
        DirtyChatTurns();
      }
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
    chat_.status = ui::String(ToolActivityLabel(event.tool_name, event.status).c_str());
    ShellSetActivity(true, chat_.status);
    DirtyChatChrome();
    break;
  case AgentEventType::AssistantDelta:
    OnAssistantDelta(event);
    break;
  case AgentEventType::AssistantReady:
    ClearStreamingRow();
    FinishAssistantReply(event.entry_id, event.text, !StructuredTextParser::IsBlocksJsonDocument(event.text),
                         event.finish_reason, event.thread_id, event.response_goal, event.render_mode,
                         event.shared_ai_mode, event.sources);
    break;
  case AgentEventType::Error:
    log().error << "Agent session error: " << event.message;
    // A partial answer was already persisted via AssistantReady; the error toast is separate.
    UserFeedback::NeedsSetup(error_message);
    if (AgentReady() && agent_ports_.has_conversation_entries && agent_ports_.has_conversation_entries() &&
        agent_ports_.last_conversation_entry_id && agent_ports_.complete_assistant_message &&
        agent_ports_.set_assistant_display_plain) {
      if (auto entry_id = agent_ports_.last_conversation_entry_id()) {
        agent_ports_.complete_assistant_message(*entry_id, error_message);
        agent_ports_.set_assistant_display_plain(*entry_id, error_message);
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
  if (!unlock_ensure_.ensure_unlocked) {
    if (action) {
      action();
    }
    return;
  }
  if (unlock_ensure_.is_unlock_in_progress && unlock_ensure_.is_unlock_in_progress()) {
    ShowToast(Tr("startup.still_preparing"));
    NotifySurfaceChanged();
  }
  unlock_ensure_.ensure_unlocked(
      [this, action = std::move(action)](const bool unlocked) {
        if (!unlocked) {
          if (!unlock_ensure_.is_unlock_in_progress || !unlock_ensure_.is_unlock_in_progress()) {
            ShowToast(Tr("chat.toast.pin_required"));
            NotifySurfaceChanged();
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
  const std::string kRegisterBriefHard = Tr("chat.hint.brief_register");
  const std::string kGuestBriefSoft = Tr("chat.hint.brief_free_tier");
  const std::string kBriefUnavailable = Tr("chat.hint.brief_unavailable");
  const std::string kBriefUnavailableTemp = Tr("chat.hint.brief_unavailable_temp");

  if (!use_llm_) {
    UserFeedback::NeedsSetup(Tr("chat.hint.mock_replies"));
    return;
  }
  if (ResolvePreset(config) == "brief") {
    EnsureBriefGuestLlmKey();
    std::string registered;
    std::string guest;
    if (MessagingReady()) {
      if (auto identity = facade_->GetIdentity()) {
        registered = identity->brief_llm_api_key;
        guest = identity->brief_llm_guest_api_key;
      }
    }
    const std::string brief_key = ResolveBriefLlmApiKey(registered, guest);
    if (brief_key.empty()) {
      // The mint hint can be the server's own wording, so remember what was shown to dismiss it later.
      last_brief_banner_ = !brief_guest_mint_user_hint_.empty() ? brief_guest_mint_user_hint_ : kBriefUnavailable;
      UserFeedback::NeedsSetup(last_brief_banner_);
      return;
    }
    if (registered.empty() && !guest.empty()) {
      last_brief_banner_ = kGuestBriefSoft;
      UserFeedback::NeedsSetup(last_brief_banner_);
      return;
    }
    const std::string& banner = ChromeSnapshot().banner_message;
    if ((!last_brief_banner_.empty() && banner == last_brief_banner_) || banner == kRegisterBriefHard ||
        banner == kGuestBriefSoft || banner == kBriefUnavailable || banner == kBriefUnavailableTemp) {
      last_brief_banner_.clear();
      if (shell_feedback_.dismiss_banner) {
        shell_feedback_.dismiss_banner();
      }
    }
    return;
  }
  if (config.llm.require_api_key && config.llm.api_key.empty()) {
    UserFeedback::NeedsSetup(Tr("chat.hint.api_key_needed"));
  }
}

void ChatController::EnsureBriefGuestLlmKey() {
  if (!MessagingReady() || !facade_) {
    return;
  }
  auto identity = facade_->GetIdentity();
  if (!identity) {
    return;
  }
  if (!identity->brief_llm_api_key.empty() || !identity->brief_llm_guest_api_key.empty()) {
    brief_guest_mint_user_hint_.clear();
    return;
  }
  if (brief_guest_mint_attempted_) {
    return;
  }
  brief_guest_mint_attempted_ = true;

  std::string base_url = Store().Snapshot().config.llm.base_url;
  if (ResolvePreset(Store().Snapshot().config) != "brief" || base_url.empty()) {
    base_url = BriefLlmBaseUrl();
  }
  auto minted = MintBriefGuestLlmKey(base_url);
  if (!minted) {
    brief_guest_mint_user_hint_ = AppError::Display(minted.error());
    if (brief_guest_mint_user_hint_.empty()) {
      brief_guest_mint_user_hint_ = Tr("chat.hint.brief_unavailable_temp");
    }
    return;
  }
  brief_guest_mint_user_hint_.clear();
  LocalIdentity updated = *identity;
  updated.brief_llm_guest_api_key = minted->llm_api_key;
  if (!facade_->UpdateLocalIdentity(updated)) {
    brief_guest_mint_user_hint_ = Tr("chat.hint.brief_key_save_failed");
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
  EnsureBriefGuestLlmKey();
  RefreshLlmSetupBanner();
  if (IThreadStore* store = facade_ ? facade_->ThreadStore() : nullptr) {
    if (agent_ports_.set_thread_store) {
      agent_ports_.set_thread_store(store);
    }
  }
  chat_.compose_disabled = false;
  DirtyChatChrome();
  facade_->SetOnMessagesChanged([this]() {
    RefreshFromMessaging();
    if (contacts_notify_.refresh) {
      contacts_notify_.refresh();
    }
  });
  facade_->SetOnDeliveryNotice([this](const std::string& message) {
    ShowToast(message);
    NotifySurfaceChanged();
  });
  facade_->SetOnBackgroundUnread(
      [this](std::string title, std::string body, std::string thread_id) {
        if (!Store().Snapshot().profile_prefs.show_notifications) {
          return;
        }
        ILocalNotifier::Instance().NotifyIncoming(title, body, thread_id);
      });
  ILocalNotifier::Instance().SetActivationHandler([this](std::string thread_id) {
    DesktopWindowChrome::RaiseAndFocus();
    if (!thread_id.empty()) {
      OnSelectThread(thread_id);
    }
  });
  // Relay poll is armed by ConversationsHub::StartCoordinatorTimers (not here). Call-wake UI
  // refresh is wired via ConversationsHub::SetOnCallWake from Application.
  IPushDeviceRegistrar::SetTokenChangedHandler([this](const std::string& /*token*/) {
    AppRuntime::PostUI([this]() {
      if (!MessagingReady()) {
        return;
      }
      if (facade_) {
        (void)facade_->SyncPushDevices(Store().Snapshot().profile_prefs.show_notifications);
      }
    });
  });
  if (facade_) {
    (void)facade_->SyncPushDevices(Store().Snapshot().profile_prefs.show_notifications);
  }
  facade_->SetOnThreadChanged([this]() {
    RefreshFromMessaging();
    if (contacts_notify_.refresh) {
      contacts_notify_.refresh();
    }
  });
  if (facade_ && facade_->HasRouter()) {
    facade_->SetOnLocalAction(
        [this](const std::string& message, const std::optional<std::string>& payload) {
          HandleLocalAction(message, payload);
        });
    facade_->SetSharedAiConfirmCallback(
        [this](const std::string& thread_id, const AtAiMode mode, const std::string& prompt,
               std::function<void(bool confirmed, bool dont_ask_again)> done) {
          ShowConfirmWithCheckbox(Tr("chat.share_ai.title"),
              Tr(mode == AtAiMode::SharedFull ? "chat.share_ai.body_full" : "chat.share_ai.body_reply"),
              Tr("chat.share_ai.dont_ask"), false,
              [this, thread_id, done = std::move(done)](const bool ok, const bool dont_ask) {
                if (ok && dont_ask) {
                  facade_->MarkSharedAiConfirmed(thread_id);
                }
                done(ok, dont_ask);
              });
        });
  }
  facade_->SetOnActionMessage([this](const std::string& message) {
    ShowToast(message);
    NotifySurfaceChanged();
  });
  RefreshFromMessaging();
  facade_->TailSyncActiveE2eThread();

  const bool auto_renew = Store().Snapshot().profile_prefs.auto_renew_registration;
  auto renew = facade_
      ? facade_->MaybeAutoRenewRegistration(auto_renew)
      : Roe<bool>(false);
  if (!renew) {
    log().warning << "Auto-renew registration failed: " << renew.error().message;
  } else if (*renew) {
    log().info << "Network registration auto-renewed";
  } else {
    auto identity = facade_->GetIdentity();
    if (identity && ShouldRenewRegistration(*identity) && !auto_renew) {
      UserFeedback::NeedsSetup(Tr("chat.hint.registration_expires"));
    }
  }
  // Always reload so Brief key from identity is applied after unlock (not only on renew).
  ReloadAgentConfig();
  RefreshLlmSetupBanner();
}

bool ChatController::Setup(ui::Context* context) {
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
      facade_->WarmPeerForThread(active);
    }
  });
  const AppConfig& config = Store().Snapshot().config;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  shell_.sessions = {{ui::String("Chat"), ui::String(Tr("chat.ai_thread.placeholder").c_str())}};
  pending_reply_.reset();
  use_llm_ = !config.llm.base_url.empty();
  StartupMark("chat_after_agent_ports");

  if (MessagingInitialized()) {
    WireMessagingBindings();
  }

  // Must go through Apply so Brief injects identity.brief_llm_api_key.
  Apply(ProjectAgent(config));
  log().info << "Chat initialized (model: " << config.llm.model << ")";

  // Do NOT DataModelHost::Clear() here. Application already registered window/settings/contacts/
  // people_picker handles; a full Clear made DirtyNavChrome/DirtyCallChrome no-ops (handle=0) while
  // MountInner still updated live Context models — mute/speaker icons stuck until remount.

  const auto register_enter_send = [this](ui::Input::KeyIdentifier key) {
    if (!input_) {
      return;
    }
    input_->Register(KeyBinding{
        .key = key,
        .forbidden_modifiers = ui::Input::KM_SHIFT,
        .when = [](ui::Context* ctx) {
          ui::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
          return focus && focus->GetId() == "draft-input";
        },
        .action = [this]() {
          OnSendMessage();
          return false;
        },
        .priority = 50,
    });
  };
  register_enter_send(ui::Input::KI_RETURN);
  register_enter_send(ui::Input::KI_NUMPADENTER);

  if (!DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
        auto& controller = *this;
        RegisterChatWidgetDataTypes(ctor);
        ctor.Bind("draft", &controller.chat_.draft);
        ctor.Bind("draft_placeholder", &controller.chat_.draft_placeholder);
        ctor.Bind("status", &controller.chat_.status);
        ctor.Bind("loading", &controller.chat_.loading);
        ctor.Bind("has_turns", &controller.chat_.has_turns);
        ctor.Bind("turns", &controller.chat_.turns);
        ctor.Bind("messages", &controller.chat_.messages);
        ctor.Bind("use_messages_layout", &controller.chat_.use_messages_layout);
        ctor.Bind("thread_title", &controller.chat_.thread_title);
        ctor.Bind("thread_subtitle", &controller.chat_.thread_subtitle);
        ctor.Bind("peer_link_status", &controller.chat_.peer_link_status);
        ctor.Bind("peer_link_banner", &controller.chat_.peer_link_banner);
        ctor.Bind("show_peer_link", &controller.chat_.show_peer_link);
        ctor.Bind("show_peer_link_banner", &controller.chat_.show_peer_link_banner);
        ctor.Bind("peer_link_direct", &controller.chat_.peer_link_direct);
        ctor.Bind("peer_link_via_hop", &controller.chat_.peer_link_via_hop);
        ctor.Bind("peer_link_via_relay", &controller.chat_.peer_link_via_relay);
        ctor.Bind("peer_link_connecting", &controller.chat_.peer_link_connecting);
        ctor.Bind("peer_link_degraded", &controller.chat_.peer_link_degraded);
        ctor.Bind("peer_link_failed", &controller.chat_.peer_link_failed);
        ctor.Bind("peer_link_ready", &controller.chat_.peer_link_ready);
        ctor.Bind("show_retry_peer_dial", &controller.chat_.show_retry_peer_dial);
        ctor.Bind("thread_encrypted", &controller.chat_.thread_encrypted);
        ctor.Bind("thread_is_ai", &controller.chat_.thread_is_ai);
        ctor.Bind("thread_is_private", &controller.chat_.thread_is_private);
        ctor.Bind("thread_is_public", &controller.chat_.thread_is_public);
        ctor.Bind("thread_is_group", &controller.chat_.thread_is_group);
        ctor.Bind("compose_disabled", &controller.chat_.compose_disabled);
        ctor.Bind("composer_input_disabled", &controller.chat_.composer_input_disabled);
        ctor.Bind("show_attach_button", &controller.chat_.show_attach_button);
        ctor.Bind("image_chip", &controller.chat_.image_chip);
        ctor.Bind("image_preparing", &controller.chat_.image_preparing);
        ctor.Bind("image_thumb_ready", &controller.chat_.image_thumb_ready);
        ctor.Bind("image_thumb_src", &controller.chat_.image_thumb_src);
        ctor.Bind("image_draft_name", &controller.chat_.image_draft_name);
        ctor.Bind("attachment_uploading", &controller.chat_.attachment_uploading);
        ctor.Bind("attachment_draft_name", &controller.chat_.attachment_draft_name);
        ctor.Bind("show_thread_actions", &controller.chat_.show_thread_actions);
        ctor.Bind("show_peer_sheet", &controller.chat_.show_peer_sheet);
        ctor.Bind("show_call_actions", &controller.chat_.show_call_actions);
        ctor.Bind("show_forget_memory", &controller.chat_.show_forget_memory);
        ctor.Bind("show_sync_with_peer", &controller.chat_.show_sync_with_peer);
        ctor.Bind("show_thread_menu", &controller.chat_.show_thread_menu);
        ctor.Bind("show_gap_banner", &controller.chat_.show_gap_banner);
        ctor.Bind("show_compromised_banner", &controller.chat_.show_compromised_banner);
        ctor.Bind("show_locked_out_banner", &controller.chat_.show_locked_out_banner);
        ctor.Bind("show_psk_setup_banner", &controller.chat_.show_psk_setup_banner);
        ctor.Bind("show_psk_import", &controller.chat_.show_psk_import);
        ctor.Bind("psk_has_key", &controller.chat_.psk_has_key);
        ctor.Bind("psk_verified", &controller.chat_.psk_verified);
        ctor.Bind("psk_fingerprint", &controller.chat_.psk_fingerprint);
        ctor.Bind("psk_export_b64", &controller.chat_.psk_export_b64);
        ctor.Bind("psk_import_text", &controller.chat_.psk_import_text);
        ctor.Bind("sync_in_progress", &controller.chat_.sync_in_progress);
        ctor.Bind("show_older_history_hint", &controller.chat_.show_older_history_hint);
        ctor.Bind("show_jump_to_latest", &controller.chat_.show_jump_to_latest);
        ctor.Bind("jump_to_latest_label", &controller.chat_.jump_to_latest_label);
        ctor.BindEventCallback("send_message", &ChatController::SendMessageCallback);
        ctor.BindEventCallback("send_suggestion", &ChatController::SendSuggestionCallback);
        ctor.BindEventCallback("send_suggestion_action", &ChatController::SendSuggestionActionCallback);
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("open_chat_link", &ChatController::OpenChatLinkCallback);
        ctor.BindEventCallback("stop_turn", &ChatController::StopTurnCallback);
        ctor.BindEventCallback("toggle_reaction", &ChatController::ToggleReactionCallback);
        ctor.BindEventCallback("open_emoji_insert", &ChatController::OpenEmojiInsertCallback);
        ctor.BindEventCallback("attach_file", &ChatController::AttachFileCallback);
        ctor.BindEventCallback("remove_image", &ChatController::RemoveImageCallback);
        ctor.BindEventCallback("open_attachment", &ChatController::OpenAttachmentCallback);
        ctor.BindEventCallback("download_attachment", &ChatController::DownloadAttachmentCallback);
        ctor.BindEventCallback("retry_attachment", &ChatController::RetryAttachmentCallback);
        ctor.BindEventCallback("submit_form", &ChatController::SubmitFormCallback);
        ctor.BindEventCallback("calendar_prev", &ChatController::CalendarPrevCallback);
        ctor.BindEventCallback("calendar_next", &ChatController::CalendarNextCallback);
        ctor.BindEventCallback("select_calendar_day", &ChatController::SelectCalendarDayCallback);
        ctor.BindEventCallback("open_working_set", &ChatController::OpenWorkingSetCallback);
        ctor.BindEventCallback("clear_history", &ChatController::ClearHistoryCallback);
        ctor.BindEventCallback("forget_memory", &ChatController::ForgetMemoryCallback);
        ctor.BindEventCallback("open_thread_actions_menu", &ChatController::OpenThreadActionsMenuCallback);
        ctor.BindEventCallback("start_call", &ChatController::StartCallCallback);
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

  if (!DataModelHost::Instance().Register(context, "shell", [this](ui::DataModelConstructor& ctor) {
        auto& controller = *this;
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
          session_handle.RegisterMember("date_label", &ChatController::SessionRow::date_label);
          session_handle.RegisterMember("active", &ChatController::SessionRow::active);
          session_handle.RegisterMember("closable", &ChatController::SessionRow::closable);
        }
        ctor.RegisterArray<std::vector<ChatController::SessionRow>>();
        ctor.Bind("sessions", &controller.shell_.sessions);
        ctor.Bind("working_set_active", &controller.shell_.working_set_active);
        ctor.Bind("working_set_title", &controller.shell_.working_set_title);
        ctor.Bind("working_set_subtitle", &controller.shell_.working_set_subtitle);
        ctor.Bind("working_set_rml", &controller.shell_.working_set_rml);
        ctor.Bind("working_set", &controller.shell_.working_set);
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

  if (shell_setup_.initialize) {
    shell_setup_.initialize(context);
  }

  ContextMenuHost::Instance().RegisterProvider([this](const ContextMenuRequest& request) {
    std::vector<ContextMenuAction> actions;
    if (!messaging_ready_ || chat_.compose_disabled) {
      return actions;
    }
    const std::string message_id = FindMessageIdFromElement(request.target);
    if (message_id.empty()) {
      return actions;
    }
    // Reactions sit in the row above the message, the rest in the list below it.
    for (const char* emoji : kReactionPresets) {
      ContextMenuAction react{std::string("react_") + emoji, emoji, nullptr,
                              [this, message_id, emoji]() { ToggleReaction(message_id, emoji); }};
      react.quick = true;
      actions.push_back(std::move(react));
    }
    ContextMenuAction more{"react_more", "+", nullptr, [this, message_id]() { ShowReactionMorePrompt(message_id); }};
    more.quick = true;
    actions.push_back(std::move(more));

    // Copy takes the pointer selection when there is one (desktop), otherwise the whole message.
    std::string text;
    if (request.context) {
      if (ui::SelectionController* selection = request.context->GetSelectionController()) {
        text = selection->GetSelectedText();
      }
    }
    if (text.empty()) {
      text = MessagePlainText(message_id);
    }
    if (!text.empty()) {
      // Reply quotes the message under what the user types; the quote travels as plain text.
      actions.push_back({"reply_message", Tr("chat.menu.reply"), nullptr, [this, text]() { ComposeWithQuote("", text); }});
      actions.push_back({"copy_message", Tr("common.copy"), nullptr, [text]() {
                           if (ui::SystemInterface* system = ui::GetSystemInterface()) {
                             system->SetClipboardText(text);
                           }
                         }});
      // "@ai " plus the quoted message go into the composer; the user adds the question and sends.
      if (auto active = facade_->GetActiveThread(); active && active->kind == ThreadKind::Direct) {
        actions.push_back({"ask_ai_message", Tr("chat.menu.ask_ai"), nullptr, [this, text]() { ComposeWithQuote("@ai ", text); }});
      }
    }
    return actions;
  });
  ContextMenuHost::Instance().SetAnchorResolver([](ui::Element* target) -> ui::Element* {
    for (ui::Element* cur = target; cur; cur = cur->GetParentNode()) {
      if (cur->HasAttribute("message-id")) {
        return cur;
      }
    }
    return nullptr;
  });

  // After Initialize clears state: Latin UI is ready; CJK waits on deferred faces.
  if (shell_setup_.set_fonts_ready) {
    shell_setup_.set_fonts_ready(!UiLanguageNeedsCjkFonts());
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
      {.key = "preview", .rml_path = "views/preview.rml", .role = PaneRole::Auxiliary, .toolbar_label = Tr("shell.pane.preview").c_str()});

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
  if (facade_) {
    mesh_ready_ = facade_->Snapshot().mesh_ready;
  } else if (messaging_ui_.snapshot) {
    mesh_ready_ = messaging_ui_.snapshot().call_ready;
  }
  chrome_.Update();
  DirtyChatChrome();
  if (ChromeSnapshot().nav_tab == NavTab::Home) {
    OnHomeTabActivated();
  }
}

void ChatController::OnMeshReady() {
  // NotifyMeshReady also fires when StartMesh/attach failed — sync the real flag.
  if (facade_) {
    mesh_ready_ = facade_->Snapshot().mesh_ready;
  } else if (messaging_ui_.snapshot) {
    mesh_ready_ = messaging_ui_.snapshot().call_ready;
  } else {
    mesh_ready_ = false;
  }
  chrome_.Update();
  DirtyChatChrome();
  NotifySurfaceChanged();
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
    EnsureBriefGuestLlmKey();
    std::string brief_key;
    if (MessagingInitialized() && MessagingReady()) {
      if (auto identity = facade_->GetIdentity()) {
        brief_key = ResolveBriefLlmApiKey(identity->brief_llm_api_key, identity->brief_llm_guest_api_key);
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
        if (MessagingInitialized() && register_messaging_tools_) {
          register_messaging_tools_(tools);
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
    facade_->SuspendMeshColdPeers();
  }
}

void ChatController::Update() {
  FlushStreamingRow();
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_) {
    if (MessagingReady()) {
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
  if (MessagingInitialized() && facade_) {
    facade_->SetOnMessagesChanged(nullptr);
    facade_->SetOnDeliveryNotice(nullptr);
    facade_->SetOnBackgroundUnread(nullptr);
    facade_->SetOnThreadChanged(nullptr);
    facade_->SetOnActionMessage(nullptr);
    facade_->SetOnLocalAction(nullptr);
    facade_->SetSharedAiConfirmCallback(nullptr);
  }
  if (AgentReady()) {
    StartupPhase phase("Shutdown::AgentSession");
    if (agent_ports_.cancel) {
      agent_ports_.cancel();
    }
    // ConfigureOnIO may still be running (and used to touch ConversationsHub via the
    // tool hook). Wait before Application tears the hub down.
    if (agent_ports_.wait_for_configure_idle) {
      agent_ports_.wait_for_configure_idle();
    }
  }
  // Hub + ProfileSecrets lifetime is owned by Application::ShutdownMessaging.
  messaging_ready_ = false;
  mesh_ready_ = false;
  pending_reply_.reset();
  streaming_.reset();
  chat_links_.clear();
  context_ = nullptr;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  use_llm_ = false;
}

} // namespace pbr
