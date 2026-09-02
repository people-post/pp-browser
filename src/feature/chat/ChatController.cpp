#include <stdexcept>
#include <filesystem>
#include "feature/chat/ChatController.h"
#include "feature/messaging/MessagingFacade.h"
#include "feature/ui/ShellSetupPorts.h"
#include "feature/chat/ChatDataModel.h"
#include "feature/chat/ChatWidgetHost.h"
#include "feature/ui/BadgeAggregator.h"
#include "feature/ui/BadgeNotifyPorts.h"
#include "base/i18n/LocalizationService.h"
#include "base/i18n/ScriptLanguageDetector.h"
#include "base/runtime/AppLifecycle.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/DesktopWindowChrome.h"
#include "base/platform/ILocalNotifier.h"
#include "base/platform/IPushDeviceRegistrar.h"
#include "base/platform/NativeFileDialog.h"
#include "base/platform/PlatformOpenFile.h"

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
#include "RmlUi_Backend.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/PeerDisplayLabel.h"
#include "base/people/ContactJson.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/messaging/AtAiParser.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/messaging/ChatPayloadValidator.h"
#include "common/chat/MessagingLimits.h"
#include "base/messaging/MessagingJson.h"
#include "base/messaging/ReactionTypes.h"
#include "base/messaging/SendRelayOptions.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/EmojiKey.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/DocumentLoader.h"
#include "feature/ui/DeferredStartup.h"
#include "feature/ui/CallActionsPorts.h"
#include "feature/ui/UnlockEnsurePorts.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/PaymentFeedback.h"
#include "feature/ui/UserFeedback.h"
#include "feature/ui/BlobQuotaRecoveryFlow.h"
#include "base/mesh/reachability/Reachability.h"
#include "base/data/Config.h"
#include "base/data/LlmPreset.h"
#include "base/data/SessionStore.h"
#include "base/error/AppError.h"
#include "base/net/BriefGuestLlmClient.h"
#include "base/platform/DeploymentProfile.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/PeoplePickerNotifyPorts.h"

#include <RmlUi/Core/SystemInterface.h>
#include "feature/ui/SettingsController.h"

#include "common/ValueJson.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

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

void ChatController::BindMessagingFacade(MessagingFacade* facade) {
  facade_ = facade;
  scroller_.BindMessagingFacade(facade);
  chrome_.BindMessagingFacade(facade);
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
  Rml::Element* el = context_->GetDocument(0)->GetElementById("draft-input");
  auto* draft = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(el);
  if (!draft) {
    return;
  }
  draft->SetValue(next.c_str());
  const Rml::String value = draft->GetValue();
  const int end = Rml::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(value.size()));
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

void ChatController::ToggleReactionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != Rml::Variant::STRING || args[1].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().ToggleReaction(std::string(args[0].Get<Rml::String>().c_str()),
                            std::string(args[1].Get<Rml::String>().c_str()));
}

void ChatController::OpenEmojiInsertCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                             const Rml::VariantList& /*args*/) {
  Instance().OpenEmojiInsertMenu(&ev);
}

void ChatController::AttachFileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().OnAttachFile();
}

void ChatController::OpenAttachmentCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OpenAttachment(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::RetryAttachmentCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().RetryAttachmentDownload(std::string(args[0].Get<Rml::String>().c_str()));
}

void ChatController::DownloadAttachmentCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().DownloadAttachment(std::string(args[0].Get<Rml::String>().c_str()));
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

void ChatController::StartCallCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                       const Rml::VariantList& /*args*/) {
  ChatController& self = Instance();
  const std::string thread_id = self.ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  self.ShowConfirmWithCheckbox(
      Tr("call.start.title"), Tr("call.start.message"), Tr("call.start.allow_video"), false,
      [thread_id](const bool ok, const bool allow_video) {
        if (!ok) {
          return;
        }
        ChatController& inner = Instance();
        if (inner.call_actions_.start_call) {
          (void)inner.call_actions_.start_call(thread_id, allow_video);
        }
      });
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
      UserFeedback::Fail("Could not delete conversation");
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
                "Transfer to " + label,
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
              "Dismiss on this device only",
              nullptr,
              [dismiss_and_close, group_id]() { dismiss_and_close(group_id); },
              "../icons/trash.svg",
              true,
          });
          ContextMenuHost::Instance().ShowActions(Rml::Vector2i(120, 120), std::move(actions));
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

  ShowConfirm("Forget what AI learned?",
      "Delete the durable conversation summary? Your message transcript stays on this device.",
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
  for (const Thread& thread : sorted_threads) {
    SessionRow row;
    row.id = thread.id.c_str();
    row.title = facade_
                    ? facade_->ResolveThreadLabel(thread).title.c_str()
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
  chat_.composer_input_disabled = chat_.compose_disabled || chat_.attachment_uploading;
}

void ChatController::OnAttachFile() {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  if (chat_.composer_input_disabled || chat_.attachment_uploading || !chat_.show_attach_button) {
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

  AppRuntime::PostWorkerNormal([this, thread_id, path]() {
    BlobQuotaRecoveryFlow::RunUpload<ThreadMessage>(
        [this, thread_id, path]() { return facade_->SendAttachmentFromPath(thread_id, path); },
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
  });
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

void ChatController::HandleLocalAction(const std::string& message, const std::optional<std::string>& payload) {
  if (payload && !payload->empty()) {
    auto action_json = TryParseObject(*payload);
    auto action_type = action_json ? action_json->getString("type") : std::nullopt;
    if (action_type && *action_type == "tool_permission") {
      const std::string approval_id = action_json->getString("approval_id").value_or("");
      const std::string decision = action_json->getString("decision").value_or("");
      if (!agent_ports_.resume_tool_permission) {
        ShowToast("Assistant is not ready for permission decisions.");
        return;
      }
      auto resumed = agent_ports_.resume_tool_permission(approval_id, decision, message);
      if (!resumed) {
        ShowToast(resumed.error().message);
      }
      return;
    }
    if (action_type && *action_type == "fork_group") {
      const std::string confirmed_payload = *payload;
      ShowConfirm("Start a new group?",
          "This creates a new group with a fresh history. People who are still reachable can be invited again.",
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

std::string FindMessageIdFromElement(Rml::Element* element) {
  for (Rml::Element* cur = element; cur; cur = cur->GetParentNode()) {
    if (cur->HasAttribute("message-id")) {
      const Rml::String value = cur->GetAttribute("message-id", Rml::String());
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
      "React", "Pick an emoji (use the keyboard emoji key on mobile).", "",
      [this, message_id](bool confirmed, std::string value) {
        if (!confirmed) {
          return;
        }
        ToggleReaction(message_id, value);
      });
}

void ChatController::OpenReactPresetMenu(const std::string& message_id, Rml::Vector2i position) {
  if (message_id.empty()) {
    return;
  }
  std::vector<ContextMenuAction> actions;
  for (const char* emoji : kReactionPresets) {
    actions.push_back({
        std::string("react_") + emoji,
        emoji,
        nullptr,
        [this, message_id, emoji]() { ToggleReaction(message_id, emoji); },
    });
  }
  actions.push_back({
      "react_more",
      "More…",
      nullptr,
      [this, message_id]() { ShowReactionMorePrompt(message_id); },
  });
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OpenEmojiInsertMenu(Rml::Event* ev) {
  if (chat_.compose_disabled) {
    return;
  }
  if (emoji_picker_notify_.open_insert) {
    emoji_picker_notify_.open_insert();
    return;
  }
  // Fallback when picker is not wired (tests / headless): keep preset strip.
  const Rml::Vector2i position = ev ? MenuPositionBelowEvent(*ev) : Rml::Vector2i(120, 120);
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
  auto thread = facade_->GetActiveThread();
  if (!thread) {
    return;
  }

  const Rml::Vector2i position = MenuPositionBelowEvent(ev);

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
          "Add people…",
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
          "View contact",
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
          "Add to contacts",
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
              UserFeedback::Fail("Could not add contact");
              NotifySurfaceChanged();
              return;
            }
            if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail("Directory hit missing Account ID");
                NotifySurfaceChanged();
                return;
              }
              facade_->RegisterPeerSigningKey(ContactIdKindToString(ContactIdKind::Account), *hit.account_id,
                                              *hit.signing_public_key_b64, "directory");
            }
            if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail("Directory hit missing Account ID");
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
          "Copy ID",
          nullptr,
          [this, peer_id]() {
            if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
              system->SetClipboardText(peer_id.c_str());
            }
            ShowToast("ID copied");
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
        "Rename for me",
        nullptr,
        [this, thread_id, current_local, shared_default]() {
          ShowPrompt("Rename for me", "Local nickname for this group",
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
          "Clear my name",
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
          "Rename for everyone",
          nullptr,
          [this]() {
            ShowToast("Only the owner can do that — see the note in the chat");
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
  Rml::Element* el = context_->GetDocument(0)->GetElementById("draft-input");
  auto* draft = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(el);
  if (!draft) {
    return;
  }
  draft->Focus();
  const Rml::String value = draft->GetValue();
  const int end = Rml::StringUtilities::ConvertByteOffsetToCharacterOffset(value, static_cast<int>(value.size()));
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

  const std::string thread_id = ActiveThreadId();
  auto messages = facade_->GetMessagesPage(thread_id, std::nullopt, 10000);
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
    AppRuntime::PostUI([this, action_message, action_payload, close_working_set]() {
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

    std::string hydrated = InjectEntryPlaceholders(parsed.rml, entry_id);
    hydrated = HydrateChatActionButtons(hydrated, chat_actions);

    const std::string assistant_open =
        ApplyLangAttribute(R"(<div class="bubble bubble-assistant")", raw_output) + R"( selectable="text">)";

    if (messaging_ready_) {
      const std::string active_thread = thread_id.empty() ? ActiveThreadId() : thread_id;
      auto messages = facade_->GetMessagesPage(active_thread, std::nullopt, 10000);
      bool updated = false;
      if (messages) {
        for (ThreadMessage& message : *messages) {
          if (message.id == entry_id) {
            message.content_rml = assistant_open + hydrated + "</div>";
            message.chat_actions = chat_actions;
            (void)facade_->UpdateMessage(message);
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
        (void)facade_->AppendMessage(ai_message);
      }
      (void)facade_->UpdatePreview(active_thread, parsed.rml);
    }

    working_set_.ApplyFromParse(entry_id, parsed.working_set_candidates);

    if (shared_ai_mode == AtAiMode::SharedReply || shared_ai_mode == AtAiMode::SharedFull) {
      const std::string active_thread =
          thread_id.empty() ? ActiveThreadId() : thread_id;
      std::string relay_plain = raw_output;
      if (StructuredTextParser::IsBlocksJsonDocument(raw_output)) {
        if (auto blocks_doc = TryParseObject(raw_output)) {
          if (const Array* blocks = blocks_doc->getArray("blocks")) {
            std::string joined;
            for (const Value& block_value : blocks->elements) {
              const Object* block = asObject(block_value);
              if (!block) {
                continue;
              }
              if (block->getString("type").value_or("") == "paragraph") {
                if (auto text = block->getString("text")) {
                  if (!joined.empty()) {
                    joined += "\n";
                  }
                  joined += *text;
                }
              }
            }
            if (!joined.empty()) {
              relay_plain = joined;
            }
          }
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
            ShowToast("PIN required to continue");
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
  constexpr const char* kRegisterBriefHard =
      "Register your identity in Me → Profile to use Brief assistant (or switch to Cloud/Ollama).";
  constexpr const char* kGuestBriefSoft =
      "Using Brief free tier — register in Me → Profile for higher limits.";

  if (!use_llm_) {
    UserFeedback::NeedsSetup("Using mock replies — LLM is not configured.");
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
      if (!brief_guest_mint_user_hint_.empty()) {
        UserFeedback::NeedsSetup(brief_guest_mint_user_hint_);
      } else {
        UserFeedback::NeedsSetup(
            "Brief free tier unavailable right now — try again later, or register in Me → Profile.");
      }
      return;
    }
    if (registered.empty() && !guest.empty()) {
      UserFeedback::NeedsSetup(kGuestBriefSoft);
      return;
    }
    const std::string& banner = ChromeSnapshot().banner_message;
    if (banner == kRegisterBriefHard || banner == kGuestBriefSoft ||
        banner.find("Brief free tier") != std::string::npos) {
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
      brief_guest_mint_user_hint_ =
          "Brief free tier is temporarily unavailable — try again later.";
    }
    return;
  }
  brief_guest_mint_user_hint_.clear();
  LocalIdentity updated = *identity;
  updated.brief_llm_guest_api_key = minted->llm_api_key;
  if (!facade_->UpdateLocalIdentity(updated)) {
    brief_guest_mint_user_hint_ = "Couldn't save Brief free-tier key — try again later.";
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
  if (facade_ && agent_ports_.with_session) {
    agent_ports_.with_session([&](AgentSession& agent) { facade_->BindAgent(agent); });
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
  // Relay poll is armed by MessagingHub::StartCoordinatorTimers (not here). Call-wake UI
  // refresh is wired via MessagingHub::SetOnCallWake from Application.
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
          const char* mode_label = mode == AtAiMode::SharedFull ? "share the prompt and reply" : "share the AI reply";
          ShowConfirmWithCheckbox("Share with peer?",
              std::string("This will ") + mode_label +
                  " on the encrypted thread. Your local @ai assist stays private.",
              "Don't ask again for this conversation", false,
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
      facade_->WarmPeerForThread(active);
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

  // Do NOT DataModelHost::Clear() here. Application already registered window/settings/contacts/
  // people_picker handles; a full Clear made DirtyNavChrome/DirtyCallChrome no-ops (handle=0) while
  // MountInner still updated live Context models — mute/speaker icons stuck until remount.

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

  if (!DataModelHost::Instance().Register(context, "chat", [this](Rml::DataModelConstructor& ctor) {
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
        ctor.Bind("show_retry_peer_dial", &controller.chat_.show_retry_peer_dial);
        ctor.Bind("thread_encrypted", &controller.chat_.thread_encrypted);
        ctor.Bind("thread_is_ai", &controller.chat_.thread_is_ai);
        ctor.Bind("thread_is_private", &controller.chat_.thread_is_private);
        ctor.Bind("thread_is_public", &controller.chat_.thread_is_public);
        ctor.Bind("thread_is_group", &controller.chat_.thread_is_group);
        ctor.Bind("compose_disabled", &controller.chat_.compose_disabled);
        ctor.Bind("composer_input_disabled", &controller.chat_.composer_input_disabled);
        ctor.Bind("show_attach_button", &controller.chat_.show_attach_button);
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
        ctor.BindEventCallback("send_chat_action", &ChatController::SendChatActionCallback);
        ctor.BindEventCallback("toggle_reaction", &ChatController::ToggleReactionCallback);
        ctor.BindEventCallback("open_emoji_insert", &ChatController::OpenEmojiInsertCallback);
        ctor.BindEventCallback("attach_file", &ChatController::AttachFileCallback);
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

  if (!DataModelHost::Instance().Register(context, "shell", [this](Rml::DataModelConstructor& ctor) {
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
    const Rml::Vector2i pos = request.position;
    actions.push_back({
        "react_message",
        "React…",
        nullptr,
        [this, message_id, pos]() { OpenReactPresetMenu(message_id, pos); },
    });
    return actions;
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

} // namespace pbr
