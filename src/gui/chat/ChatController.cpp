#include <chrono>
#include <thread>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include "gui/chat/ChatController.h"

#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/AttachmentBackfillPolicy.h"
#include "common/Metrics.h"
#include "feature/conversations/ConversationsFacade.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ChatAnswer.h"
#include "feature/conversations/SessionListText.h"
#include "feature/conversations/SessionMenu.h"
#include "gui/chat/BriefFeedIntent.h"
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
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/PlainTextLinks.h"
#include "lib/messaging/QuoteReply.h"
#include "foundation/platform/PlatformOpenFile.h"
#include "foundation/platform/PlatformOpenUrl.h"

#include "lib/ai/MarkdownToRml.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/WorkingSetPolicy.h"
#include "lib/ai/conversation/Conversation.h"
#include "foundation/platform/IAssetLocator.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/InputCoordinator.h"
#include "lib/ui/ChatFormHelper.h"
#include "lib/ui/RmlVariantHelpers.h"
#include "lib/ui/CalendarHelper.h"
#include "gui/chat/ChatWidgetStateBuilder.h"
#include "common/ui/WorkingSetCodec.h"
#include "common/Utilities.h"
#include "common/StartupTiming.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "lib/messaging/GroupTypes.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/people/ContactJson.h"
#include "domain/registration/RegistrationClient.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/ChatPayloadValidator.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/MessagingJson.h"
#include "lib/messaging/ReactionTypes.h"
#include "lib/messaging/SendRelayOptions.h"
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
#include "lib/ui/PaymentFeedback.h"
#include "gui/UserFeedback.h"
#include "gui/BlobQuotaRecoveryFlow.h"
#include "mesh/reachability/Reachability.h"
#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/data/SessionStore.h"
#include "foundation/error/AppError.h"
#include "lib/net/BriefGuestLlmClient.h"
#include "foundation/platform/DeploymentProfile.h"
#include "lib/ui/ContextMenuHost.h"
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
#include "common/MeshAlias.h"

namespace pbr {

static_assert(kSessionMutedForever == kThreadMutedForever,
              "SessionMenu.h and ThreadLocalPrefsStore.h must agree on \"always\"");

namespace {


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
  presenter_.BindCallbacks({
      .on_changed = [this]() { ApplySessionList(); },
      .prefs_failed = []() { UserFeedback::Fail(Tr("sidebar.error.prefs_failed")); },
  });
  scroller_.SetDirtyTurns([]() { DirtyChatTurns(); });
  working_set_.SetWidgetLookup([this](const std::string& entry_id) -> const TurnWidgetState* {
    return widgets_.Find(entry_id);
  });
  chrome_.SetAiAttachInfo([this]() {
    ChatThreadChrome::AiAttachInfo info;
    info.brief_preset = ResolvePreset(Store().Snapshot().config) == "brief";
    info.ai_usable = messaging_ready_ && facade_ && agent_.AgentCloudReady();
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
  presenter_.BindFacade(facade);
  scroller_.BindConversationsFacade(facade);
  chrome_.BindConversationsFacade(facade);
  scroller_.SetRowBuilder([this](const std::string& thread_id, std::optional<int64_t> oldest,
                                 std::optional<int64_t> newest) { return BuildMessageRows(thread_id, oldest, newest); });
}

void ChatController::BindRegisterMessagingTools(std::function<void(ToolRegistry&)> hook) {
  register_messaging_tools_ = std::move(hook);
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

void ChatController::ShellSyncLayout(const bool restore_focus_after, const std::source_location where) {
  if (shell_navigation_.request_sync_layout) {
    const std::string_view file = where.file_name();
    const size_t slash = file.find_last_of("/\\");
    const std::string reason =
        std::string(slash == std::string_view::npos ? file : file.substr(slash + 1)) + ":" + std::to_string(where.line());
    shell_navigation_.request_sync_layout(restore_focus_after, reason.c_str());
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
  // A Home chip continues its own thread instead of opening a new one on every tap.
  Instance().SendUserText(std::string(args[0].Get<ui::String>().c_str()), std::nullopt, /*from_home_chip=*/true);
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
  Instance().SendUserText(Tr("home.suggestion." + id + "_prompt"), std::move(payload), /*from_home_chip=*/true);
}

void ChatController::SubmitFormCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                  const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING || args[1].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().agent_.SubmitForm(std::string(args[0].Get<ui::String>().c_str()),
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

  Instance().agent_.SendChatAction(std::string(args[0].Get<ui::String>().c_str()), *action_index);
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
  Instance().agent_.OnStopTurn();
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
  Instance().agent_.CalendarPrev(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::CalendarNextCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                    const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().agent_.CalendarNext(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::SelectCalendarDayCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                         const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING || args[1].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().agent_.SelectCalendarDay(std::string(args[0].Get<ui::String>().c_str()),
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
  presenter_.ShowHome();
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
  if (presenter_.SelectThread(thread_id)) {
    ShellSetPrimaryPane("chat");
    FinalizeThreadDisplay();
  }
}

void ChatController::CloseChatWith(const std::string& thread_id, const std::function<Roe<void>()>& action,
                                   const bool show_error_text) {
  // Closing may change the active thread, so decide before it: a row closed from the sessions list
  // may not be the open chat, and only the open chat has a draft, a reply in flight or widgets to drop.
  const bool was_open = thread_id == ActiveThreadId();
  if (auto done = action(); !done) {
    UserFeedback::Fail(show_error_text ? done.error().message : Tr("chat.error.delete_failed"));
    NotifySurfaceChanged();
    return;
  }
  if (was_open) {
    chat_.draft = "";
    chat_.status = "";
    chat_.loading = false;
    pending_reply_.reset();
    working_set_.ClearAll();
    widgets_.ClearAll();
    chat_.turns.clear();
  }
  RefreshFromMessaging();
  if (shell_.sessions.empty()) {
    ShellSelectNavTab(NavTab::Home);
    ShellCloseCompactChat();
  }
  ShellSyncLayout();
  NotifySurfaceChanged();
}

void ChatController::OnCloseThread(const std::string& thread_id) {
  if (!messaging_ready_) {
    return;
  }
  const CloseChatPlan plan = presenter_.PlanClose(thread_id);
  const std::string group_id = plan.group_id;
  auto confirm_then = [this](const std::string& title_key, const std::string& body_key, std::function<void()> run) {
    ShowConfirm(Tr(title_key), Tr(body_key), [run = std::move(run)](const bool ok) {
      if (ok) {
        run();
      }
    });
  };
  switch (plan.kind) {
  case CloseChatPlan::Kind::Delete:
  case CloseChatPlan::Kind::DeleteAi:
    confirm_then("chat.delete_conversation",
                 plan.kind == CloseChatPlan::Kind::DeleteAi ? "chat.delete_confirm_ai" : "chat.delete_confirm",
                 [this, thread_id]() { CloseChatWith(thread_id, [this, thread_id]() { return presenter_.CloseThread(thread_id); }); });
    return;
  case CloseChatPlan::Kind::Dismiss:
    confirm_then("chat.group.dismiss_title", "chat.group.dismiss_confirm", [this, thread_id, group_id]() {
      CloseChatWith(thread_id, [this, thread_id, group_id]() { return presenter_.DismissGroupAndClose(thread_id, group_id); });
    });
    return;
  case CloseChatPlan::Kind::Leave:
    confirm_then("chat.group.leave_title", "chat.group.leave_confirm", [this, thread_id, group_id]() {
      CloseChatWith(thread_id, [this, thread_id, group_id]() { return presenter_.LeaveGroupAndClose(thread_id, group_id); });
    });
    return;
  case CloseChatPlan::Kind::LeaveAsOwner:
    confirm_then("chat.group.leave_title", "chat.group.leave_owner_confirm",
                 [this, thread_id, group_id, successors = plan.successors]() {
                   std::vector<ContextMenuAction> actions;
                   for (const std::string& successor : successors) {
                     std::string label = successor;
                     if (auto contact = facade_->FindContactByIdentity(successor, mesh::ContactIdKind::Account);
                         contact && *contact) {
                       label = (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
                       if (label.empty()) {
                         label = successor;
                       }
                     }
                     actions.push_back({
                         "transfer_" + successor,
                         Tr("chat.group.transfer_to", {{"name", label}}),
                         nullptr,
                         [this, thread_id, group_id, successor]() {
                           CloseChatWith(
                               thread_id,
                               [this, thread_id, group_id, successor]() {
                                 return presenter_.HandOverAndClose(thread_id, group_id, successor);
                               },
                               /*show_error_text=*/true);
                         },
                         "../icons/contacts.svg",
                     });
                   }
                   actions.push_back({
                       "dismiss_local",
                       Tr("chat.group.dismiss_local"),
                       nullptr,
                       [this, thread_id, group_id]() {
                         CloseChatWith(thread_id, [this, thread_id, group_id]() {
                           return presenter_.DismissGroupAndClose(thread_id, group_id);
                         });
                       },
                       "../icons/trash.svg",
                       true,
                   });
                   ContextMenuHost::Instance().ShowActions(ui::Vector2i(120, 120), std::move(actions));
                   NotifySurfaceChanged();
                 });
    return;
  }
}

void ChatController::AfterThreadPrefsChanged() {
  RefreshFromMessaging();
  NotifySurfaceChanged();
}

void ChatController::OpenMuteMenu(const std::string& thread_id, const ui::Vector2i position) {
  static constexpr std::pair<SessionMuteChoice, const char*> kChoices[] = {
      {SessionMuteChoice::EightHours, "sidebar.menu.mute_8h"},
      {SessionMuteChoice::OneWeek, "sidebar.menu.mute_1w"},
      {SessionMuteChoice::Always, "sidebar.menu.mute_always"},
  };
  std::vector<ContextMenuAction> actions;
  for (const auto& [choice, label_key] : kChoices) {
    actions.push_back({
        std::string("session_") + label_key,
        Tr(label_key),
        nullptr,
        [this, thread_id, choice = choice]() {
          presenter_.Mute(thread_id, choice);
          AfterThreadPrefsChanged();
        },
    });
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::OnToggleArchived() {
  presenter_.ToggleArchived();
  RefreshFromMessaging();
  NotifySurfaceChanged();
}

void ChatController::ToggleArchivedCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                            const ui::VariantList& /*args*/) {
  Instance().OnToggleArchived();
}

std::vector<ContextMenuAction> ChatController::SessionMenuActions(const std::string& thread_id,
                                                                  const ui::Vector2i position) {
  std::vector<ContextMenuAction> actions;
  auto found = facade_->GetThread(thread_id);
  if (!found || !*found) {
    return actions;
  }
  const Thread& thread = **found;

  // Same rule as the chat header's "View contact": the resolved contact, else the first saved participant.
  std::optional<std::string> contact_id;
  if (thread.kind == ThreadKind::Direct) {
    contact_id = facade_->ResolveThreadLabel(thread).contact_id;
    if (!contact_id && !thread.participant_contact_ids.empty() && !thread.participant_contact_ids.front().empty()) {
      const std::string& candidate = thread.participant_contact_ids.front();
      if (auto contact = facade_->GetContact(candidate); contact && *contact) {
        contact_id = candidate;
      }
    }
  }

  SessionMenuState state;
  state.kind = thread.kind == ThreadKind::Group ? SessionKind::Group
               : thread.kind == ThreadKind::Direct ? SessionKind::Direct
                                                   : SessionKind::Ai;
  state.has_contact = contact_id.has_value();
  if (contact_id) {
    if (auto contact = facade_->GetContact(*contact_id); contact && *contact) {
      state.blocked = (*contact)->local.trust == TrustLevel::Blocked;
    }
  }
  const ThreadLocalPrefs prefs = presenter_.PrefsOf(thread_id).value_or(ThreadLocalPrefs{});
  state.unread = SessionIsUnread(thread.unread_count, prefs.marked_unread);
  state.pinned = prefs.pinned_at != 0;
  state.muted = ThreadIsMuted(prefs.muted_until, util::NowUnixMs());
  state.archived = prefs.archived;

  for (const SessionMenuItem item : SessionMenuItems(state)) {
    ContextMenuAction action;
    action.danger = SessionMenuItemIsDestructive(item);
    switch (item) {
    case SessionMenuItem::MarkUnread:
      action.id = "session_mark_unread";
      action.label = Tr("sidebar.menu.mark_unread");
      // A mark, not a message count: the row shows a dot and keeps its place in the list.
      action.run = [this, thread_id]() {
        presenter_.MarkUnread(thread_id);
        AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::MarkRead:
      action.id = "session_mark_read";
      action.label = Tr("sidebar.menu.mark_read");
      action.run = [this, thread_id]() {
        presenter_.MarkRead(thread_id);
        AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::Pin:
    case SessionMenuItem::Unpin:
      action.id = item == SessionMenuItem::Pin ? "session_pin" : "session_unpin";
      action.label = Tr(item == SessionMenuItem::Pin ? "sidebar.menu.pin" : "sidebar.menu.unpin");
      action.icon = "../icons/pin.svg";
      action.run = [this, thread_id, pin = item == SessionMenuItem::Pin]() {
        presenter_.SetPinned(thread_id, pin);
        AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::Mute:
      action.id = "session_mute";
      action.label = Tr("sidebar.menu.mute");
      action.icon = "../icons/bell-off.svg";
      action.run = [this, thread_id, position]() { OpenMuteMenu(thread_id, position); };
      break;
    case SessionMenuItem::Unmute:
      action.id = "session_unmute";
      action.label = Tr("sidebar.menu.unmute");
      action.icon = "../icons/bell-off.svg";
      action.run = [this, thread_id]() {
        presenter_.Unmute(thread_id);
        AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::Archive:
    case SessionMenuItem::Unarchive:
      action.id = item == SessionMenuItem::Archive ? "session_archive" : "session_unarchive";
      action.label = Tr(item == SessionMenuItem::Archive ? "sidebar.menu.archive" : "sidebar.menu.unarchive");
      action.icon = "../icons/archive.svg";
      action.run = [this, thread_id, archive = item == SessionMenuItem::Archive]() {
        presenter_.SetArchived(thread_id, archive);
        AfterThreadPrefsChanged();
      };
      break;
    case SessionMenuItem::ViewContact:
      action.id = "session_view_contact";
      action.label = Tr("chat.menu.view_contact");
      action.icon = "../icons/contacts.svg";
      action.run = [this, id = *contact_id]() {
        if (contacts_notify_.select_contact) {
          contacts_notify_.select_contact(id);
        }
      };
      break;
    case SessionMenuItem::Block:
    case SessionMenuItem::Unblock: {
      const bool block = item == SessionMenuItem::Block;
      action.id = block ? "session_block" : "session_unblock";
      action.label = Tr(block ? "chat.menu.block" : "chat.menu.unblock");
      action.run = [this, id = *contact_id, block]() {
        const auto apply = [this, id, block]() {
          if (auto set = facade_->SetContactTrust(id, block ? TrustLevel::Blocked : TrustLevel::Unknown); !set) {
            UserFeedback::Fail(Tr("contacts.err.trust"));
            return;
          }
          if (contacts_notify_.refresh) {
            contacts_notify_.refresh();
          }
          ShowToast(Tr(block ? "chat.menu.blocked_toast" : "chat.menu.unblocked_toast"));
          NotifySurfaceChanged();
        };
        if (!block) {
          apply();
          return;
        }
        ShowConfirm(Tr("chat.menu.block_confirm_title"), Tr("chat.menu.block_confirm_body"), [apply](const bool ok) {
          if (ok) {
            apply();
          }
        });
      };
      break;
    }
    case SessionMenuItem::ClearChat:
      action.id = "session_clear_chat";
      action.label = Tr("chat.menu.clear_history");
      action.run = [this, thread_id]() { ClearHistoryOf(thread_id); };
      break;
    case SessionMenuItem::Delete:
      action.id = "session_delete";
      action.label = Tr("sidebar.menu.delete");
      action.icon = "../icons/trash.svg";
      // The close flow asks before it deletes.
      action.run = [this, thread_id]() { OnCloseThread(thread_id); };
      break;
    case SessionMenuItem::LeaveGroup:
      action.id = "session_leave_group";
      action.label = Tr("chat.group.leave_title");
      // The close flow asks by itself and, for an owner, hands the group over first.
      action.run = [this, thread_id]() { OnCloseThread(thread_id); };
      break;
    }
    actions.push_back(std::move(action));
  }
  return actions;
}

void ChatController::OnClearHistory() { ClearHistoryOf(ActiveThreadId()); }

void ChatController::ClearHistoryOf(const std::string& thread_id) {
  if (!messaging_ready_ || thread_id.empty()) {
    return;
  }

  std::optional<Thread> thread;
  if (auto found = facade_->GetThread(thread_id); found && *found) {
    thread = **found;
  }
  // Clearing is local only, like deleting. An AI thread has no other side: clearing it also forgets what
  // the assistant learned in it, so nothing of the cleared chat lingers (Kenneth 2026-10-06: no checkbox).
  const bool is_ai = thread && thread->kind == ThreadKind::Ai;
  std::string message = Tr(is_ai ? "chat.clear_history.body_ai" : "chat.clear_history.body");
  if (thread && thread->kind == ThreadKind::Direct && thread->encrypted) {
    message += " " + Tr("chat.clear_history.body_secure");
  }

  ShowConfirm(Tr("chat.clear_history"), message, [this, thread_id, is_ai](bool ok) {
    if (!ok) {
      return;
    }
    if (!facade_->ClearThreadHistory(thread_id, /*forget_memory=*/is_ai)) {
      return;
    }
    AfterHistoryCleared(thread_id);
  });
}

void ChatController::AfterHistoryCleared(const std::string& thread_id) {
  // A thread cleared from the sessions list may not be the open one; only the open chat has state to reset.
  if (thread_id == ActiveThreadId()) {
    chat_.draft = "";
    chat_.status = "";
    chat_.loading = false;
    pending_reply_.reset();
    widgets_.ClearAll();
  }
  RefreshFromMessaging();
  NotifySurfaceChanged();
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
  // profile.db is recreated: nothing read from the old one may be written back into the new one.
  presenter_.ResetForNewProfile();
  shell_.showing_archived = false;
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
  presenter_.Refresh();  // ApplySessionList copies it in
}

void ChatController::ApplySessionList() {
  const ChatListScreen& list = presenter_.List();
  shell_.sessions.clear();
  shell_.archived_count = list.archived_count;
  shell_.showing_archived = list.showing_archived;
  shell_.archived_entry_label =
      list.showing_archived ? Tr("sidebar.archived.back").c_str()
                            : Tr("sidebar.archived.entry", {{"count", std::to_string(list.archived_count)}}).c_str();
  for (const ChatSessionRow& from : list.rows) {
    SessionRow row;
    row.id = from.id.c_str();
    // Both are bound as text ({{session.title}}), so names and message text need no escaping here.
    row.title = from.title_is_default_ai ? Tr("chat.new_chat").c_str() : from.title.c_str();
    row.preview = from.preview.c_str();
    row.kind = from.kind.c_str();
    row.unread_count = from.unread_count;
    row.unread_display = FormatBadgeCount(from.unread_count).c_str();
    row.date_label = from.date_label.c_str();
    row.active = from.active;
    row.pinned = from.pinned;
    row.muted = from.muted;
    row.unread_dot = from.unread_dot;
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
    agent_.OnAttachAiImage();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  auto on_picked = [this, thread_id](std::vector<std::string> paths) {
    AppRuntime::PostUI([this, thread_id, paths = std::move(paths)]() mutable {
      if (paths.empty()) {
        return;
      }
      StartAttachmentUpload(std::move(paths.front()));
    });
  };
  if (!Platform::IsMobile()) {
    ShowOpenFileDialog(Backend::GetWindow(), std::move(on_picked));
    return;
  }
  ShowAttachChoice([on_picked]() { ShowOpenImageFileDialog(Backend::GetWindow(), on_picked); },
                   [on_picked]() { ShowOpenFileDialog(Backend::GetWindow(), on_picked); });
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
      [this, path](Roe<ThreadMessage> sent) {
        ReleasePickedFile(path); // read (and kept as a CAS copy) by now
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


void ChatController::ShowAttachChoice(std::function<void()> photo, std::function<void()> file) {
  // A phone keeps photos and files apart: ask which (the desktop dialog shows both at once).
  std::vector<ContextMenuAction> actions;
  actions.push_back({"attach_photo", Tr("chat.attach.photo"), nullptr, std::move(photo)});
  actions.push_back({"attach_file", Tr("chat.attach.file"), nullptr, std::move(file)});
  actions.push_back({"attach_cancel", Tr("common.cancel"), nullptr, []() {}});
  ContextMenuHost::Instance().ShowActions(ui::Vector2i(120, 120), std::move(actions));
}

namespace {


} // namespace


void ChatController::OnRemoveImage() {
  agent_.DiscardPendingAiImage();
}



std::vector<MessageDisplayRow> ChatController::BuildMessageRows(const std::string& thread_id,
                                                                std::optional<int64_t> oldest_inclusive,
                                                                std::optional<int64_t> newest_inclusive) const {
  if (!facade_) {
    return {};
  }
  std::vector<MessageDisplayRow> rows = RenderTranscript(
      facade_->BuildTranscript(thread_id, oldest_inclusive, newest_inclusive), facade_->TranscriptFactsFor());
  agent_.DecorateAiImageRows(rows);
  return rows;
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

namespace {
constexpr int64_t kSlowDisplaySyncMs = 8; // half a 60 Hz frame
} // namespace

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  if (!facade_ || !facade_->GetActiveThread()) {
    chrome_.ResetPanelState();
    return;
  }
  const auto sync_started = std::chrono::steady_clock::now();
  const std::string thread_id = ActiveThreadId();
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id);
  // The attachment backfill reads the whole thread, so it does not run on every sync. New messages are
  // queued where they are received or sent; the backfill also re-queues failed downloads, which is why
  // it still runs now and then while the thread stays open (AttachmentBackfillPolicy.h).
  int64_t backfill_ms = 0;
  if (ShouldBackfillAttachments(thread_changed, sync_started, last_attachment_backfill_)) {
    last_attachment_backfill_ = sync_started;
    facade_->EnsureThreadAttachments(thread_id);
    backfill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  }
  if (thread_changed) {
    agent_.DiscardPendingAiImage(); // the chip belongs to the composer of the thread it was picked in
    CancelQuoteReply();      // so does a pending reply
  }

  const std::string prev_tail_id =
      chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
  const size_t prev_count = chat_.messages.size();

  chat_.messages =
      BuildMessageRows(thread_id, scroller_.LoadedMinDisplayOrder(), scroller_.LoadedMaxDisplayOrder());
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;

  scroller_.EndDisplaySync(thread_changed, prev_tail_id, prev_count);

  // Runs on the UI thread for every message, reaction and AI reply: report the slow ones.
  const auto sync_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  if (sync_ms >= kSlowDisplaySyncMs) {
    MetricsLine("chat.sync_slow")
        .Add("ms", static_cast<int64_t>(sync_ms))
        .Add("backfill_ms", backfill_ms) // the whole-thread attachment check, when it ran in this sync
        .Add("rows", static_cast<int64_t>(chat_.messages.size()))
        .Add("thread_changed", thread_changed ? "1" : "0")
        .Emit();
  }
}

// The delta's entry_id is the pending turn's user message id; the final AssistantReady carries the
// persisted assistant message id, so the live bubble is a synthetic last row that ClearStreamingRow
// drops right before FinishAssistantReply rebuilds the rows.




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
  if (!IsOpenableUrl(url)) {
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
      if (!agent_.Ports().resume_tool_permission) {
        ShowToast(Tr("chat.permission.not_ready"));
        return;
      }
      auto resumed = agent_.Ports().resume_tool_permission(approval_id, decision, message);
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
  const bool with_image = agent_.HasPendingImage() && InAiComposerContext();
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
    if (auto valid = ChatPresenter::ValidateDraft(text); !valid) {
      ShowToast(Tr(valid.error().message));
      return;
    }
  }

  if (with_image) {
    // The draft is cleared only once the question is really on its way; a refused send keeps the text and the chip.
    if (agent_.SendImageQuestion(text)) {
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
  const std::string out = presenter_.ComposeWithQuote(text);
  CancelQuoteReply();
  SendUserText(out);
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
  agent_.Cancel();
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

/** The sessions-list row the pointer is on (`session-id` is set by sidebar.rml), or empty. */
std::string FindSessionIdFromElement(ui::Element* element) {
  for (ui::Element* cur = element; cur; cur = cur->GetParentNode()) {
    if (cur->HasAttribute("session-id")) {
      const ui::String value = cur->GetAttribute("session-id", ui::String());
      return std::string(value.c_str());
    }
  }
  return {};
}

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
  if (!messaging_ready_) {
    return;
  }
  if (auto sent = presenter_.ToggleReaction(message_id, emoji); !sent) {
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

namespace {

// What the user reads in a message.
std::string PlainTextOf(const ThreadMessage& message) {
  // Local-pipeline AI answers are stored as UI block documents.
  if (const auto prose = StructuredTextParser::PlainTextIfBlocks(message.text)) {
    return *prose;
  }
  // A reply that quotes another message: only what this message itself says.
  if (const auto parts = SplitQuoteReply(message.text)) {
    return parts->reply;
  }
  return message.text;
}

} // namespace

void ChatController::OnDeleteMessage(const std::string& message_id, const ui::Vector2i position) {
  if (!facade_) {
    return;
  }
  auto delete_for_me = [this, message_id]() {
    if (auto deleted = presenter_.DeleteForMe(message_id); !deleted) {
      ShowToast(AppError::Display(deleted.error()));
      return;
    }
    SyncDisplayFromThread();
    DirtyChatTurns();
    NotifySurfaceChanged();
  };
  if (!presenter_.CanDeleteForEveryone(message_id)) {
    ShowConfirm(Tr("chat.menu.delete_title"), Tr("chat.menu.delete_confirm"), [delete_for_me](const bool ok) {
      if (ok) {
        delete_for_me();
      }
    });
    return;
  }
  std::vector<ContextMenuAction> actions;
  ContextMenuAction everyone{"delete_for_everyone", Tr("chat.menu.delete_for_everyone"), nullptr,
                             [this, message_id]() { DeleteMessageForEveryone(message_id); }};
  everyone.danger = true;
  actions.push_back(std::move(everyone));
  ContextMenuAction mine{"delete_for_me", Tr("chat.menu.delete_for_me"), nullptr, delete_for_me};
  mine.danger = true;
  actions.push_back(std::move(mine));
  actions.push_back({"delete_cancel", Tr("common.cancel"), nullptr, []() {}});
  // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
  AppRuntime::PostUI([position, actions = std::move(actions)]() mutable {
    ContextMenuHost::Instance().ShowActions(position, std::move(actions));
  });
}

void ChatController::DeleteMessageForEveryone(const std::string& message_id) {
  presenter_.DeleteForEveryone(message_id, [this](Roe<void> done) {
    if (!done) {
      ShowToast(AppError::Display(done.error()));
      return;
    }
    SyncDisplayFromThread();
    DirtyChatTurns();
    NotifySurfaceChanged();
  });
}

bool ChatController::IsDeletedMessage(const std::string& message_id) const {
  if (!facade_ || message_id.empty()) {
    return false;
  }
  auto found = facade_->GetMessage(ActiveThreadId(), message_id);
  if (!found || !found->has_value()) {
    return false;
  }
  const ThreadMessage& message = **found;
  return message.content_type == ChatContentType::Deleted;
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
    if (message.id == message_id) {
      return PlainTextOf(message);
    }
  }
  return {};
}

void ChatController::StartQuoteReply(const std::string& text, const std::string& draft_prefix) {
  if (chat_.compose_disabled || text.empty()) {
    return;
  }
  presenter_.StartQuoteReply(text);
  // One line in the bar, however long or multi-line the message is.
  chat_.quote_reply = true;
  chat_.quote_reply_text = StructuredTextParser::EscapeText(presenter_.QuotePreview()).c_str(); // bound with data-rml
  DataModelHost::Instance().Dirty("chat", "quote_reply");
  DataModelHost::Instance().Dirty("chat", "quote_reply_text");
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  auto* draft = ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById("draft-input"));
  if (!draft) {
    return;
  }
  if (!draft_prefix.empty()) {
    chat_.draft = draft_prefix.c_str();
    DataModelHost::Instance().Dirty("chat", "draft");
    draft->SetValue(draft_prefix.c_str());
  }
  draft->Focus();
  if (!draft_prefix.empty()) {
    const ui::String current = draft->GetValue();
    const int end = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(current, static_cast<int>(current.size()));
    draft->SetSelectionRange(end, end);
  }
}

void ChatController::CancelQuoteReply() {
  if (!chat_.quote_reply && !presenter_.QuotingReply()) {
    return;
  }
  presenter_.CancelQuoteReply();
  chat_.quote_reply = false;
  chat_.quote_reply_text = "";
  DataModelHost::Instance().Dirty("chat", "quote_reply");
  DataModelHost::Instance().Dirty("chat", "quote_reply_text");
}

void ChatController::OpenShareTargets(const std::string& text, const ui::Vector2i position) {
  if (!facade_ || text.empty()) {
    return;
  }
  // One friend per share, no caption: the direct chats, most recent first.
  constexpr size_t kMaxShareTargets = 30;
  std::vector<Thread> targets;
  if (auto threads = facade_->ListThreads()) {
    for (const Thread& thread : *threads) {
      if (thread.kind == ThreadKind::Direct && !(thread.local_title.empty() && thread.title.empty())) {
        targets.push_back(thread);
      }
    }
  }
  std::sort(targets.begin(), targets.end(), [](const Thread& a, const Thread& b) { return a.updated_at > b.updated_at; });
  if (targets.size() > kMaxShareTargets) {
    targets.resize(kMaxShareTargets);
  }
  if (targets.empty()) {
    ShowToast(Tr("chat.share.no_friends"));
    return;
  }
  std::vector<ContextMenuAction> actions;
  for (const Thread& thread : targets) {
    const std::string name = thread.local_title.empty() ? thread.title : thread.local_title;
    actions.push_back({"share_to_" + thread.id, name, nullptr, [this, thread_id = thread.id, name, text]() {
                         SendRelayOptions opts;
                         opts.update_preview = true;
                         if (auto sent = facade_->SendUserMessage(thread_id, text, opts); !sent) {
                           ShowToast(sent.error().message);
                           return;
                         }
                         ShowToast(Tr("chat.share.sent", {{"name", name}}));
                       }});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::JumpToQuotedMessage(const std::string& reply_message_id) {
  if (!facade_ || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  auto page = facade_->GetMessagesPage(ActiveThreadId(), std::nullopt, 500);
  if (!page) {
    return;
  }
  // The quote travels as text, so the original is found by its text: the nearest earlier message that
  // starts with the quoted words.
  std::string target_id;
  for (size_t i = 0; i < page->size(); ++i) {
    if ((*page)[i].id != reply_message_id) {
      continue;
    }
    const auto parts = SplitQuoteReply((*page)[i].text);
    if (!parts) {
      return;
    }
    std::string needle = parts->quote;
    if (needle.size() >= 3 && needle.compare(needle.size() - 3, 3, "\xE2\x80\xA6") == 0) {
      needle.erase(needle.size() - 3); // the ellipsis of a cut quote
    }
    const int64_t reply_order = (*page)[i].display_order;
    int64_t best_order = -1;
    for (const ThreadMessage& candidate : *page) {
      if (candidate.id == reply_message_id || candidate.display_order >= reply_order ||
          candidate.display_order <= best_order || candidate.content_type == ChatContentType::Annotation) {
        continue;
      }
      if (PlainTextOf(candidate).rfind(needle, 0) == 0) {
        best_order = candidate.display_order;
        target_id = candidate.id;
      }
    }
    break;
  }
  ui::Element* row = target_id.empty() ? nullptr
                                       : context_->GetDocument(0)->QuerySelector(("[message-id=\"" + target_id + "\"]").c_str());
  if (!row) {
    ShowToast(Tr("chat.quote.not_found"));
    return;
  }
  row->ScrollIntoView(true);
}

void ChatController::OpenMessageLink(const std::string& message_id, const int link_index) {
  if (!facade_ || link_index < 0) {
    return;
  }
  auto page = facade_->GetMessagesPage(ActiveThreadId(), std::nullopt, 500);
  if (!page) {
    return;
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id) {
      continue;
    }
    // The same scan the bubble was rendered from; the URL comes from the stored text, never the markup.
    const std::vector<PlainTextLink> links = FindPlainTextLinks(message.text);
    if (static_cast<size_t>(link_index) < links.size()) {
      ConfirmAndOpenUrl(links[static_cast<size_t>(link_index)].url); // https only, host confirmed first
    }
    return;
  }
}

void ChatController::OpenMessageLinkCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                             const ui::VariantList& args) {
  if (args.size() < 2 || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  const std::optional<int> link_index = EventArgAsInt(args, 1);
  if (!link_index) {
    return;
  }
  Instance().OpenMessageLink(std::string(args[0].Get<ui::String>().c_str()), *link_index);
}

void ChatController::JumpToQuoteCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/, const ui::VariantList& args) {
  if (args.empty() || args[0].GetType() != ui::Variant::STRING) {
    return;
  }
  Instance().JumpToQuotedMessage(std::string(args[0].Get<ui::String>().c_str()));
}

void ChatController::CancelQuoteReplyCallback(ui::DataModelHandle /*model*/, ui::Event& /*ev*/,
                                              const ui::VariantList& /*args*/) {
  Instance().CancelQuoteReply();
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

void ChatController::RefreshCallJoinable(const std::chrono::steady_clock::time_point now) {
  if (now - last_call_joinable_poll_ < std::chrono::seconds(1)) {
    return;
  }
  last_call_joinable_poll_ = now;
  const std::string& thread_id = ActiveThreadId();
  const bool joinable = chat_.thread_is_group && chat_.show_call_actions && !thread_id.empty() &&
                        call_actions_.call_joinable && call_actions_.call_joinable(thread_id);
  if (joinable != chat_.call_joinable) {
    chat_.call_joinable = joinable;
    DataModelHost::Instance().Dirty("chat", "call_joinable");
  }
}

void ChatController::OnStartCall(ui::Event& ev) {
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty() || !call_actions_.start_call) {
    return;
  }
  auto start_call = call_actions_.start_call;
  if (chat_.call_joinable) {
    (void)start_call(thread_id, false);  // Join: into the call that goes on, no voice / video choice
    return;
  }
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
            mesh::DirectoryHit hit;
            if (auto shadow = facade_->GetDirectoryShadow(peer_id)) {
              hit = *shadow;
            } else {
              hit.hit_id = peer_id;
              if (peer_id.rfind("account:", 0) == 0) {
                hit.account_id = peer_id;
                hit.ids = {{mesh::ContactIdKind::Account, peer_id, true}};
              } else {
                hit.ids = {{mesh::ContactIdKind::RelayUser, peer_id, false}};
              }
            }
            auto created = facade_->AddContactFromDirectoryHit(hit);
            if (!created) {
              UserFeedback::Fail(Tr("chat.error.add_contact_failed"));
              NotifySurfaceChanged();
              return;
            }
            if (hit.nickname.empty()) {
              facade_->EnsureDirectoryLookup(peer_id); // the hit fills the nickname in when it arrives
            }
            if (hit.signing_public_key_b64 && !hit.signing_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail(Tr("chat.error.directory_missing_account"));
                NotifySurfaceChanged();
                return;
              }
              facade_->RegisterPeerSigningKey(ContactIdKindToString(mesh::ContactIdKind::Account), *hit.account_id,
                                              *hit.signing_public_key_b64, "directory");
            }
            if (hit.kem_public_key_b64 && !hit.kem_public_key_b64->empty()) {
              if (!hit.account_id || hit.account_id->empty()) {
                UserFeedback::Fail(Tr("chat.error.directory_missing_account"));
                NotifySurfaceChanged();
                return;
              }
              facade_->RegisterPeerKemKey(ContactIdKindToString(mesh::ContactIdKind::Account), *hit.account_id,
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
                facade_->FindContactByIdentity(unreachable_id, mesh::ContactIdKind::Account)) {
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
  chat_.draft = Tr("home.suggestion.find_someone_prompt");
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












void ChatController::UpdateSidebarPreview(const std::string& preview_text) {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  (void)facade_->UpdatePreview(thread_id, preview_text);
  SyncShellSessions();
  DirtyShell();
}

bool ChatController::EnsureHomeOutboundSession(const std::string& chip_message) {
  if (!messaging_ready_) {
    return false;
  }
  // A chip's earlier thread (same first sentence, so the same title) is reopened; typed text starts a new one.
  std::string reuse_id;
  if (!chip_message.empty()) {
    if (auto threads = facade_->ListThreads()) {
      reuse_id = FindChipThreadId(
          *threads, chip_message, [](const Thread& t) { return t.kind == ThreadKind::Ai; },
          [](const Thread& t) -> const std::string& { return t.title; }, [](const Thread& t) { return t.updated_at; },
          [](const Thread& t) -> const std::string& { return t.id; });
    }
  }
  if (!reuse_id.empty()) {
    if (auto opened = facade_->OpenThread(reuse_id); !opened) {
      log().warning << "Home: could not reopen thread " << reuse_id << ": " << opened.error().message;
      return false;
    }
  } else if (auto created = facade_->CreateNewAiThread(); !created) {
    log().warning << "Home: could not create an AI thread: " << created.error().message;
    return false;
  }
  ShellSelectNavTab(NavTab::Sessions);
  ShellSetPrimaryPane("chat");
  FinalizeThreadDisplay();
  NotifySurfaceChanged();
  return true;
}

void ChatController::SendUserText(const std::string& text, std::optional<std::string> user_payload,
                                  bool from_home_chip) {
  const std::string trimmed = util::Trim(text);
  if (trimmed.empty()) {
    log().info << "Send ignored: empty text";
    return;
  }
  if (chat_.loading) {
    log().info << "Send ignored: a reply is still loading";
    return;
  }
  if (chat_.compose_disabled && messaging_ready_) {
    log().info << "Send ignored: the composer is disabled in this thread";
    return;
  }
  if (!messaging_ready_) {
    // The unlock can finish much later (PIN entry), so the chip origin travels with the deferred send.
    WithSecrets([this, trimmed, user_payload = std::move(user_payload), from_home_chip]() mutable {
      SendUserText(trimmed, std::move(user_payload), from_home_chip);
    });
    return;
  }

  // "给我今天最新的简报" / "latest briefs" typed to the AI is the Brief feed, the same as the Home chip:
  // the app fetches it itself. The sentence's language picks the feed, whatever the UI language.
  if (!user_payload && InAiComposerContext()) {
    if (const BriefFeedRequest feed = MatchBriefFeedRequest(trimmed); feed.matched) {
      user_payload = SuggestionPayload("articles", feed.chinese ? "zh" : "en");
    }
  }

  if (ChromeSnapshot().nav_tab == NavTab::Home) {
    if (!EnsureHomeOutboundSession(from_home_chip ? trimmed : std::string())) {
      log().info << "Send ignored: could not open a thread from Home";
      return;
    }
  }

  widgets_.ExpireOpenForms();
  DirtyChatTurns();

  const bool use_mock_reply = !agent_.UseLlm();
  bool expect_agent_work = use_mock_reply;
  if (!expect_agent_work && messaging_ready_ && facade_ && facade_->HasRouter()) {
    expect_agent_work = facade_->ExpectsAgentWork(
        ActiveThreadId(), trimmed, user_payload);
  } else if (!expect_agent_work) {
    expect_agent_work = true;
  }

  // Peer relay send only needs messaging_ready; AI / Brief / mock turns need agent_cloud_ready.
  if (expect_agent_work && !agent_.AgentCloudReady()) {
    // Right after launch (and after unlock applies the Brief key) the agent is reconfiguring for about a
    // second: hold the send and show that it is connecting, instead of dropping the tap.
    if (agent_.AgentReady() && Store().IsInitialized() && !agent_.AgentConfigured() && agent_.AgentHasCredentials()) {
      log().info << "Send held: the assistant is still connecting";
      agent_.HoldSend({.text = trimmed, .payload = std::move(user_payload), .from_home_chip = from_home_chip,
                       .thread_id = ActiveThreadId()});
      UserFeedback::NeedsSetup(Tr("chat.status.connecting"), /*sticky=*/true);
      return;
    }
    log().info << "Send ignored: the assistant is not set up";
    agent_.RefreshLlmSetupBanner();
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

  if (!agent_.UseLlm()) {
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
  if (agent_.Ports().submit) {
    agent_.Ports().submit(trimmed, std::move(user_payload));
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



void ChatController::WireMessagingBindings() {
  if (!MessagingInitialized() || !agent_.AgentReady()) {
    return;
  }
  // Identity / Brief key / push registration are only valid after vault unlock.
  if (!MessagingReady()) {
    return;
  }
  messaging_ready_ = true;
  // Pre-send image previews are session files; a crash or a kill leaves them behind, so start clean.
  if (const std::string profile_dir = facade_ ? facade_->ProfileDataDir() : std::string(); !profile_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(AttachmentViewRoot(profile_dir, kAiImageViewThread), ec);
  }
  agent_.EnsureBriefGuestLlmKey();
  agent_.RefreshLlmSetupBanner();
  if (IThreadStore* store = facade_ ? facade_->ThreadStore() : nullptr) {
    if (agent_.Ports().set_thread_store) {
      agent_.Ports().set_thread_store(store);
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
  facade_->SetOnDeliveryNotice([this](const DeliveryNotice& notice) {
    ShowToast(DeliveryNoticeText(notice));
    NotifySurfaceChanged();
  });
  facade_->SetOnBackgroundUnread(
      [this](std::string title, std::string body, std::string thread_id) {
        if (!Store().Snapshot().profile_prefs.show_notifications) {
          return;
        }
        // A muted thread still counts as unread in the list; it just does not notify.
        const std::optional<ThreadLocalPrefs> prefs = presenter_.PrefsOf(thread_id);
        if (prefs && ThreadIsMuted(prefs->muted_until, util::NowUnixMs())) {
          return;
        }
        // A stranger's first message arrives before the directory answered, so the title would be a bare
        // id. Give the lookup a moment and use whatever name is known by then.
        bool bare_id = false;
        if (auto thread = facade_->GetThread(thread_id); thread && *thread) {
          bare_id = facade_->ResolveThreadLabel(**thread).trust == PeerLabelTrust::RawId;
        }
        if (!bare_id) {
          ILocalNotifier::Instance().NotifyIncoming(title, body, thread_id);
          return;
        }
        AppRuntime::PostWorkerAndReplyOnUI<int>(
            WorkerLane::Normal,
            []() {
              std::this_thread::sleep_for(std::chrono::milliseconds(1500));
              return 0;
            },
            [this, body, thread_id, title](int) {
              std::string resolved = title;
              if (auto thread = facade_->GetThread(thread_id); thread && *thread) {
                const std::string now = facade_->ResolveThreadLabel(**thread).title;
                if (!now.empty()) {
                  resolved = now;
                }
              }
              ILocalNotifier::Instance().NotifyIncoming(resolved, body, thread_id);
            });
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
        (void)facade_->SyncPushDevices();
      }
    });
  });
  if (facade_) {
    (void)facade_->SyncPushDevices();
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
  agent_.ReloadAgentConfig();
  agent_.RefreshLlmSetupBanner();
}

bool ChatController::Setup(ui::Context* context) {
  StartupPhase setup_phase("ChatController::Setup");
  if (!context) {
    return false;
  }

  context_ = context;
  // The mesh's own background / foreground reactions are the hub's (ConversationsHub::OnAppBackground).
  lifecycle_background_id_ = AppLifecycle::AddBackgroundListener([this]() { OnApplicationPause(); });
  const AppConfig& config = Store().Snapshot().config;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
  presenter_.ResetForNewProfile();
  shell_.sessions = {{ui::String("Chat"), ui::String(Tr("chat.ai_thread.placeholder").c_str())}};
  pending_reply_.reset();
  agent_.SetUseLlm(!config.llm.base_url.empty());
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
        ctor.Bind("quote_reply", &controller.chat_.quote_reply);
        ctor.Bind("quote_reply_text", &controller.chat_.quote_reply_text);
        ctor.Bind("attachment_uploading", &controller.chat_.attachment_uploading);
        ctor.Bind("attachment_draft_name", &controller.chat_.attachment_draft_name);
        ctor.Bind("show_thread_actions", &controller.chat_.show_thread_actions);
        ctor.Bind("show_peer_sheet", &controller.chat_.show_peer_sheet);
        ctor.Bind("show_call_actions", &controller.chat_.show_call_actions);
        ctor.Bind("call_joinable", &controller.chat_.call_joinable);
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
        ctor.BindEventCallback("cancel_quote_reply", &ChatController::CancelQuoteReplyCallback);
        ctor.BindEventCallback("jump_to_quote", &ChatController::JumpToQuoteCallback);
        ctor.BindEventCallback("open_message_link", &ChatController::OpenMessageLinkCallback);
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
          session_handle.RegisterMember("pinned", &ChatController::SessionRow::pinned);
          session_handle.RegisterMember("muted", &ChatController::SessionRow::muted);
          session_handle.RegisterMember("unread_dot", &ChatController::SessionRow::unread_dot);
        }
        ctor.RegisterArray<std::vector<ChatController::SessionRow>>();
        ctor.Bind("sessions", &controller.shell_.sessions);
        ctor.Bind("archived_count", &controller.shell_.archived_count);
        ctor.Bind("showing_archived", &controller.shell_.showing_archived);
        ctor.Bind("archived_entry_label", &controller.shell_.archived_entry_label);
        ctor.BindEventCallback("toggle_archived", &ChatController::ToggleArchivedCallback);
        ctor.Bind("working_set_active", &controller.shell_.working_set_active);
        ctor.Bind("working_set_title", &controller.shell_.working_set_title);
        ctor.Bind("working_set_subtitle", &controller.shell_.working_set_subtitle);
        ctor.Bind("working_set_rml", &controller.shell_.working_set_rml);
        ctor.Bind("working_set", &controller.shell_.working_set);
        ctor.BindEventCallback("new_chat", &ChatController::NewChatCallback);
        ctor.BindEventCallback("new_message", &ChatController::NewMessageCallback);
        ctor.BindEventCallback("open_new_session_menu", &ChatController::OpenNewSessionMenuCallback);
        ctor.BindEventCallback("select_thread", &ChatController::SelectThreadCallback);
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
    if (!messaging_ready_) {
      return actions;
    }
    if (const std::string session_id = FindSessionIdFromElement(request.target); !session_id.empty()) {
      return SessionMenuActions(session_id, request.position);
    }
    if (chat_.compose_disabled) {
      return actions;
    }
    // An item the user can pass on (an article in a feed list): Ask AI / Copy / Share. The nearest of
    // such an item and a message row wins.
    for (ui::Element* cur = request.target; cur && !cur->HasAttribute("message-id"); cur = cur->GetParentNode()) {
      if (!cur->HasAttribute("share-text")) {
        continue;
      }
      std::string shared = cur->GetAttribute("share-text", ui::String()).c_str();
      const std::string url = cur->GetAttribute("share-url", ui::String()).c_str();
      if (!url.empty()) {
        // "<label><url>": a PP bubble shows the label as the link (PlainTextLinks.h); pasted elsewhere it
        // still reads as a label followed by the address.
        shared += "\n" + Tr("feed.result.open") + url;
      }
      // In a direct chat the question needs the "@ai " prefix; an AI thread takes it as it is.
      auto active = facade_ ? facade_->GetActiveThread() : Roe<Thread>(Error("no facade"));
      const std::string prefix = active && active->kind == ThreadKind::Direct ? "@ai " : "";
      // The list panel closes so the composer with the quote bar is what the user sees next.
      actions.push_back({"ask_ai_item", Tr("chat.menu.ask_ai"), nullptr, [this, shared, prefix]() {
                           AppRuntime::PostUI([this, shared, prefix]() {
                             working_set_.Clear();
                             StartQuoteReply(shared, prefix);
                           });
                         }});
      actions.push_back({"copy_item", Tr("common.copy"), nullptr, [shared]() {
                           if (ui::SystemInterface* system = ui::GetSystemInterface()) {
                             system->SetClipboardText(shared);
                           }
                         }});
      const ui::Vector2i position = request.position;
      // Opened after this menu is gone: a menu cannot replace itself from inside its own click.
      actions.push_back({"share_item", Tr("chat.menu.share"), nullptr, [this, shared, position]() {
                           AppRuntime::PostUI([this, shared, position]() { OpenShareTargets(shared, position); });
                         }});
      return actions;
    }

    const std::string message_id = FindMessageIdFromElement(request.target);
    if (message_id.empty() || IsDeletedMessage(message_id)) {
      return actions; // a deleted message takes no reaction, reply, copy or second delete
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
      // Reply: the message shows as a quote bar above the composer and is attached to the next send.
      actions.push_back({"reply_message", Tr("chat.menu.reply"), nullptr, [this, text]() { StartQuoteReply(text); }});
      actions.push_back({"copy_message", Tr("common.copy"), nullptr, [text]() {
                           if (ui::SystemInterface* system = ui::GetSystemInterface()) {
                             system->SetClipboardText(text);
                           }
                         }});
      // Ask AI: the message goes into the quote bar and "@ai " into the input; the user adds the question.
      if (auto active = facade_->GetActiveThread(); active && active->kind == ThreadKind::Direct) {
        actions.push_back({"ask_ai_message", Tr("chat.menu.ask_ai"), nullptr, [this, text]() { StartQuoteReply(text, "@ai "); }});
      }
    }
    // Delete: our own message in a chat offers "for everyone" too; anything else is "for me" only.
    ContextMenuAction remove{"delete_message", Tr("common.delete"), nullptr,
                             [this, message_id, position = request.position]() { OnDeleteMessage(message_id, position); }};
    remove.danger = true;
    actions.push_back(std::move(remove));
    return actions;
  });
  ContextMenuHost::Instance().SetAnchorResolver([](ui::Element* target) -> ui::Element* {
    for (ui::Element* cur = target; cur; cur = cur->GetParentNode()) {
      if (cur->HasAttribute("message-id") || cur->HasAttribute("share-text")) {
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
    agent_.RefreshLlmSetupBanner();
  } else if (messaging_ready_) {
    agent_.RefreshLlmSetupBanner();
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




void ChatController::OnApplicationPause() {
  agent_.Cancel();
}


void ChatController::Update() {
  if (pending_reply_) {
    PendingReply reply = std::move(*pending_reply_);
    pending_reply_.reset();
    agent_.FinishAssistantReply(reply.entry_id, reply.output, reply.from_llm, {}, reply.thread_id);
  }

  if (messaging_ready_) {
    if (MessagingReady()) {
      const auto now = std::chrono::steady_clock::now();
      if (chrome_.MaybePollPeerLink(now)) {
        DirtyChatHeader();
      }
      RefreshCallJoinable(now);
    }
  }

  agent_.Tick();  // the streamed row, a held send, the agent's events
}

void ChatController::AfterLayout() {
  scroller_.ApplyPolicy();
}

void ChatController::Shutdown() {
  AppLifecycle::RemoveListener(lifecycle_background_id_);
  lifecycle_background_id_ = 0;
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
  if (agent_.AgentReady()) {
    StartupPhase phase("Shutdown::AgentSession");
    if (agent_.Ports().cancel) {
      agent_.Ports().cancel();
    }
    // ConfigureOnIO may still be running (and used to touch ConversationsHub via the
    // tool hook). Wait before Application tears the hub down.
    if (agent_.Ports().wait_for_configure_idle) {
      agent_.Ports().wait_for_configure_idle();
    }
  }
  // Hub + ProfileSecrets lifetime is owned by Application::ShutdownMessaging.
  messaging_ready_ = false;
  mesh_ready_ = false;
  pending_reply_.reset();
  agent_.Reset();
  chat_links_.clear();
  context_ = nullptr;
  widgets_.ClearAll();
  chat_ = {};
  shell_ = {};
}

} // namespace pbr
