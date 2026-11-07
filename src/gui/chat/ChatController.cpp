#include <atomic>
#include <chrono>
#include <limits>
#include <thread>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include "gui/chat/ChatController.h"
#include "gui/chat/ApprovalCard.h"
#include "gui/chat/AttachMenuRules.h"

#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/OutgoingAttachmentPrep.h"
#include "gui/AttachmentTypeLabel.h"
#include "gui/chat/VoiceBubble.h"
#include "gui/chat/VoiceSendPolicy.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_touch.h>
#include "lib/voice/VoiceSupport.h"
#include "foundation/platform/PickedFileName.h"
#include "gui/chat/AttachmentBackfillPolicy.h"
#include "common/Metrics.h"
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/MessageRouter.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/shell/FavoritesRowModel.h"
#include "lib/ui/ShellLayout.h"
#include "lib/ui/EmojiCatalog.h"
#include "gui/chat/ChatDataModel.h"
#include "gui/chat/AiImageAttach.h"
#include "gui/chat/ChatAnswer.h"
#include "gui/chat/NotificationBody.h"
#include "feature/ai/ArticleFeedFetch.h"
#include "feature/presenters/HomeToday.h"
#include "feature/presenters/SessionListText.h"
#include "feature/presenters/SessionMenu.h"
#include "feature/presenters/ThreadSearch.h"
#include "gui/chat/ChatSearchRows.h"
#include "gui/chat/BriefFeedIntent.h"
#include "gui/chat/SuggestionPayload.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/BadgeAggregator.h"
#include "gui/BadgeNotifyPorts.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/i18n/ScriptLanguageDetector.h"
#include "foundation/runtime/AppLifecycle.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/ui/DesktopWindowChrome.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "foundation/platform/Platform.h"
#include "common/IPushDeviceRegistrar.h"
#include "foundation/platform/NativeFileDialog.h"
#include "lib/messaging/DeleteForEveryone.h"
#include "lib/messaging/MentionAlert.h"
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
#include "lib/ui/DataEvent.h"
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
#include "lib/people/AvatarGlyph.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/PeerDisplayLabel.h"
#include "lib/people/PeopleLookupFailure.h"
#include "lib/people/ContactJson.h"
#include "domain/registration/RegistrationClient.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ChatPayloadValidator.h"
#include "common/chat/MessagingLimits.h"
#include "common/chat/MessagingJson.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/SendRelayOptions.h"
#include "common/thread/SyncStateTypes.h"
#include "common/thread/ThreadTypes.h"
#include "common/EmojiKey.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/DocumentLoader.h"
#include "gui/shell/DeferredStartup.h"
#include "gui/CallActionsPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/chat/PhotoPreview.h"
#include "gui/shell/ShellHost.h"
#include "gui/SettingsController.h"
#include "lib/ui/PaymentFeedback.h"
#include "gui/UserFeedback.h"
#include "feature/product/DemoDataSeeder.h"
#include "foundation/product/DevTools.h"
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
#include <ui/base/Tween.h>
#include <ui/style/Property.h>
#include <ui/style/Transform.h>
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
              "SessionMenu.h and IThreadLocalPrefsStore.h must agree on \"always\"");

namespace {

/** The desktop menu's Cancel row; the phone sheet leaves it out (its close disc, the scrim or a drag down cancel). */
ContextMenuAction AttachCancelAction() {
  ContextMenuAction cancel{"attach_cancel", Tr("common.cancel"), nullptr, []() {}};
  cancel.cancel = true;
  return cancel;
}

/** Search inside a chat: typing settles this long before the query is applied (plan: 250 ms, 400 if IME jumps). */
constexpr auto kSearchDebounce = std::chrono::milliseconds(250);

/** Milliseconds of a monotonic clock, for the search index's rebuild spacing. */
int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** What the user sees when a row action (add / message) fails: the directory's state, never raw HTTP text. */
std::string LocalActionFailureText(const Error& error) {
  if (error.message == kGroupInviteExpiredError) {
    return Tr(kGroupInviteExpiredError);
  }
  switch (ClassifyPeopleLookupError(error)) {
  case PeopleLookupFailure::NotFound:
    return Tr("contacts.err.user_not_found");
  case PeopleLookupFailure::DirectoryUnreachable:
    return Tr("contacts.err.directory_unreachable");
  case PeopleLookupFailure::Other:
    break;
  }
  // Transport / HTTP detail goes to the log at the call sites, never to the user.
  return Tr("contacts.err.add");
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
              { "label": "Message", "message": "Start chat with Alice", "payload": "{\"type\":\"start_conversation\",\"account_id\":\"account:alice123\",\"hit_id\":\"hit_alice\",\"nickname\":\"alice\",\"display_name\":\"Alice Example\"}" },
              { "label": "Add contact", "message": "Add Alice", "payload": "{\"type\":\"add_contact\",\"account_id\":\"account:alice123\",\"hit_id\":\"hit_alice\",\"nickname\":\"alice\",\"display_name\":\"Alice Example\"}" }
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
                 .thread_phone_subtitle = chat_.thread_phone_subtitle,
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
                 .thread_avatar_letter = chat_.thread_avatar_letter,
                 .thread_avatar_tone = chat_.thread_avatar_tone,
                 .thread_icon_src = chat_.thread_icon_src,
                 .thread_has_icon = chat_.thread_has_icon,
                 .compose_disabled = chat_.compose_disabled,
                 .show_attach_button = chat_.show_attach_button,
                 .show_thread_actions = chat_.show_thread_actions,
                 .show_peer_sheet = chat_.show_peer_sheet,
                 .show_call_actions = chat_.show_call_actions,
                 .show_forget_memory = chat_.show_forget_memory,
                 .show_sync_with_peer = chat_.show_sync_with_peer,
                 .show_thread_menu = chat_.show_thread_menu,
                 .show_search = chat_.show_search,
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
  VoiceComposerPorts voice_ports;
  voice_ports.limits = [this]() { return facade_ ? facade_->CurrentAttachmentLimits() : AttachmentLimits{}; };
  voice_ports.out_dir = [this]() {
    // blobs_view/voice-rec under a reserved name that is not a chat: wiped at start and when the vault locks.
    return facade_ ? (std::filesystem::path(AttachmentViewRoot(facade_->ProfileDataDir(), kVoiceViewThread)) / "voice-rec")
                         .string()
                   : std::string();
  };
  voice_ports.keep_file = [this]() { return failed_voice_path_; };
  voice_ports.thread_id = [this]() { return ActiveThreadId(); };
  voice_ports.send = [this](const std::string& thread_id, const std::string& path) {
    if (chat_.attachment_uploading || chat_.attachment_preparing) {
      KeepVoiceForRetry(path, thread_id); // another send is on: this one waits behind Retry
      UserFeedback::Fail(Tr("chat.voice.upload_busy"));
      return;
    }
    StartAttachmentUpload(path, OutgoingPickSource::File, /*voice_file=*/true, thread_id);
  };
  voice_ports.eligible = [this]() {
    return messaging_ready_ && facade_ && chat_.show_attach_button && !InAiComposerContext() &&
           !chat_.attachment_uploading && !chat_.attachment_preparing && !chat_.attachment_failed &&
           !ActiveThreadId().empty();
  };
  voice_ports.dirty = [this]() {
    SyncComposerInputState(); // the input is off while recording
    DirtyChatChrome();
    DirtyChatHeader();
  };
  voice_ports.say = [](const std::string& text) { UserFeedback::Fail(text); };
  // The call stack's own state (a call ringing here or going on), not who happens to hold a device.
  voice_ports.call_live = [this]() { return facade_ && facade_->AnyCallLive(); };
  voice_ports.fingers_down = []() {
    // The touch fingers that are down on a touch screen (not a trackpad): the hold is the finger that pressed.
    std::vector<uint64_t> ids;
    int devices = 0;
    SDL_TouchID* touch = SDL_GetTouchDevices(&devices);
    for (int i = 0; touch && i < devices; ++i) {
      if (SDL_GetTouchDeviceType(touch[i]) != SDL_TOUCH_DEVICE_DIRECT) {
        continue;
      }
      int count = 0;
      SDL_Finger** fingers = SDL_GetTouchFingers(touch[i], &count);
      for (int j = 0; fingers && j < count; ++j) {
        ids.push_back(fingers[j]->id);
      }
      SDL_free(fingers);
    }
    SDL_free(touch);
    return ids;
  };
  voice_ports.stop_playback_then = [this](std::function<void()> then) { voice_playback_->StopThen(std::move(then)); };
  voice_ = std::make_unique<VoiceComposer>(chat_, std::move(voice_ports));
  group_link_ = std::make_unique<GroupLinkSheet>(MakeGroupLinkPorts());
  join_prompt_ = std::make_unique<GroupJoinPrompt>(MakeJoinPromptPorts());

  VoicePlaybackPorts playback_ports;
  playback_ports.max_seconds = [this]() {
    return facade_ ? facade_->CurrentAttachmentLimits().voice_max_seconds : AttachmentLimits{}.voice_max_seconds;
  };
  playback_ports.resolve = [this](const std::string& message_id,
                                  std::function<void(std::optional<std::string>)> done) {
    if (!facade_) {
      done(std::nullopt);
      return;
    }
    const std::string thread_id = ActiveThreadId();
    // Making the plaintext view of a stored copy is blocking work: off the UI thread.
    AppRuntime::PostWorkerNormal([this, thread_id, message_id, done = std::move(done)]() mutable {
      bool made_view = false;
      auto path = facade_->AttachmentLocalPathForMessage(thread_id, message_id, &made_view);
      AppRuntime::PostUI([this, made_view, done = std::move(done), path = std::move(path)]() mutable {
        done(std::move(path));
        if (made_view) {
          SyncDisplayFromThread(); // a large sound file's card can now read its title and cover from the view
        }
      });
    });
  };
  playback_ports.element = [this](const std::string& element_id) -> ui::Element* {
    return context_ && context_->GetDocument(0) ? context_->GetDocument(0)->GetElementById(element_id) : nullptr;
  };
  playback_ports.say = [](const std::string& text) { UserFeedback::Fail(text); };
  playback_ports.call_live = [this]() { return facade_ && facade_->AnyCallLive(); };
  voice_playback_ = std::make_unique<VoicePlayback>(std::move(playback_ports));

  presenter_.BindCallbacks({
      .on_changed = [this]() { ApplySessionList(); },
      .prefs_failed = []() { UserFeedback::Fail(Tr("sidebar.error.prefs_failed")); },
      .prose = [](const std::string& text) { return StructuredTextParser::PlainText(text); },
  });
  scroller_.SetDirtyTurns([]() { DirtyChatTurns(); });
  working_set_.SetWidgetLookup([this](const std::string& entry_id) -> const TurnWidgetState* {
    return widgets_.Find(entry_id);
  });
  working_set_.SetEntryLoader([this](const std::string& entry_id) -> std::optional<WorkingSetController::StoredEntry> {
    if (!messaging_ready_ || !facade_ || ActiveThreadId().empty()) {
      return std::nullopt;
    }
    auto found = facade_->GetMessage(ActiveThreadId(), entry_id);
    if (!found || !found->has_value() || !(*found)->working_set_json || (*found)->working_set_json->empty()) {
      log().warning << "Working set: entry " << entry_id << " has no stored panel in thread " << ActiveThreadId();
      return std::nullopt;
    }
    return WorkingSetController::StoredEntry{WorkingSetCandidatesFromJson(*(*found)->working_set_json),
                                             (*found)->chat_actions};
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
  if (facade_) {
    // The server space is filling up: a plain line, said once per level (the hub counts).
    facade_->SetOnBlobSpaceWarning([this](const int percent) {
      ShowToast(Tr("chat.attachment.space_warning", {{"percent", std::to_string(percent)}}));
    });
  }
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
                                 std::function<void(bool)> on_result, const bool destructive) {
  if (destructive && shell_feedback_.show_confirm_destructive) {
    shell_feedback_.show_confirm_destructive(title, message, std::move(on_result), {});
  } else if (shell_feedback_.show_confirm) {
    shell_feedback_.show_confirm(title, message, std::move(on_result), {});
  }
}

void ChatController::ShowConfirmWithCheckbox(const std::string& title, const std::string& message,
                                             const std::string& checkbox_label, const bool checkbox_default,
                                             std::function<void(bool, bool)> on_result, const std::string& ok_label) {
  if (shell_feedback_.show_confirm_with_checkbox) {
    shell_feedback_.show_confirm_with_checkbox(title, message, checkbox_label, checkbox_default, std::move(on_result),
                                               ok_label);
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


void ChatController::SendSuggestionAction(const std::string& id) {
  auto payload = SuggestionPayload(id, LocalizationService::Instance().ResolvedLanguage());
  if (!payload) {
    return;
  }
  // The bubble shows the localized sentence; the payload makes the app run the function itself.
  SendUserText(Tr("home.suggestion." + id + "_prompt"), std::move(payload), /*from_home_chip=*/true);
}

void ChatController::FinalizeThreadDisplay() {
  working_set_.ClearAll();
  RefreshFromMessaging();
  RestoreWorkingSetsFromActiveThread();
  // Always land on latest when opening/showing a thread (incl. re-open same id).
  scroller_.RequestScrollToLatest();
  // The phone's chat overlay opens over the Chats tab, and over the AI 助手 page (Home) for an AI conversation —
  // so an AI chat opened/started there stays on that page and Back returns to it (not to the Chats list).
  if (ChromeSnapshot().layout_mode == LayoutMode::Compact &&
      (ChromeSnapshot().nav_tab == NavTab::Sessions ||
       (ChromeSnapshot().nav_tab == NavTab::Home && InAiComposerContext()))) {
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
  SyncDraftOwner();
  RefreshFromMessaging();
  ShellSyncLayout();
  NotifySurfaceChanged();
  RefreshHomeBrief();
}

void ChatController::OnSessionsTabActivated() {
  working_set_.ClearAll();
}

void ChatController::OnCompactChatShownAgain() {
  if (!messaging_ready_) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }
  // An answer or a message counted unread (and notified) while the hidden tab covered the chat is read now.
  presenter_.MarkRead(thread_id);
  ILocalNotifier::Instance().ClearForThread(thread_id);
  RefreshFromMessaging();
  scroller_.RequestScrollToLatest();
  NotifySurfaceChanged();
}

void ChatController::OnSelectThread(std::string thread_id) {  // by value: ShellSelectNavTab below rebuilds Home (B161)
  if (!messaging_ready_) {
    return;
  }
  if (thread_id != search_thread_) {
    CloseSearch(); // another chat: the search and its index belong to the one left
  }
  // A row of Home's "New messages" widget (a human chat) goes to Chats first, as a tap in the Chats list would, so
  // the chat overlay opens there. An AI conversation tapped on the AI 助手 page stays on that page (the overlay
  // opens over Home in FinalizeThreadDisplay), so Back returns to the AI list.
  const bool selecting_ai = facade_ && [&] {
    auto thread = facade_->GetThread(thread_id);
    return thread && *thread && (*thread)->kind == ThreadKind::Ai;
  }();
  if (!selecting_ai && ShellLayout::OpenChatSwitchesToChats(ChromeSnapshot().layout_mode, ChromeSnapshot().nav_tab)) {
    ShellSelectNavTab(NavTab::Sessions);
  }
  if (presenter_.SelectThread(thread_id)) {
    ShellSetPrimaryPane("chat");
    FinalizeThreadDisplay();
    mentions_.SyncThread();  // a group chat: its @ lines are read now, before anyone types `@`
    // Opened at its oldest unread mention (G015), read before the open dropped the marker.
    if (const std::optional<std::string>& mention = presenter_.OpenedAtMention()) {
      (void)scroller_.JumpToMessage(*mention);
    }
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
  (void)agent_.TakeQueued(thread_id); // a question queued for a closed chat has nowhere to go
  ForgetComposerExtras(thread_id);
  if (was_open) {
    CloseSearch();
    chat_.draft = "";
    chat_.status = "";
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
    }, true);
  };
  switch (plan.kind) {
  case CloseChatPlan::Kind::Delete:
  case CloseChatPlan::Kind::DeleteAi:
    confirm_then("chat.delete_conversation",
                 plan.kind == CloseChatPlan::Kind::DeleteAi ? "chat.delete_confirm_ai" : "chat.delete_confirm",
                 [this, thread_id, ai = plan.kind == CloseChatPlan::Kind::DeleteAi]() {
                   CloseChatWith(thread_id, [this, thread_id, ai]() {
                     if (ai) {
                       // An AI conversation: stop its turn first (a late answer must not bring the thread back), and
                       // clear what the AI remembered of it.
                       agent_.ForgetThread(thread_id);
                       if (facade_) {
                         (void)facade_->ForgetThreadMemory(thread_id);
                       }
                     }
                     return presenter_.CloseThread(thread_id);
                   });
                 });
    return;
  case CloseChatPlan::Kind::Dismiss:
    confirm_then("chat.group.dismiss_title", "chat.group.dismiss_confirm", [this, thread_id, group_id]() {
      CloseChatWith(thread_id, [this, thread_id, group_id]() { return presenter_.DismissGroupAndClose(thread_id, group_id); });
    });
    return;
  case CloseChatPlan::Kind::Dissolve:
    // Phones: the owner's sheet says nobody can take over and offers Dissolve (with its own confirm).
    if (AskAiCompact() && group_leave_.Open(thread_id)) {
      return;
    }
    DissolveGroupAfterConfirm(group_id);
    return;
  case CloseChatPlan::Kind::Leave:
    confirm_then("chat.group.leave_title", "chat.group.leave_confirm", [this, thread_id, group_id]() {
      CloseChatWith(thread_id, [this, thread_id, group_id]() { return presenter_.LeaveGroupAndClose(thread_id, group_id); });
    });
    return;
  case CloseChatPlan::Kind::LeaveAsOwner:
    // Phones: choose the new owner in a sheet; the one confirm comes after the pick, naming them.
    if (AskAiCompact() && group_leave_.Open(thread_id)) {
      return;
    }
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
                       "dissolve",
                       Tr("chat.group.dissolve"),
                       nullptr,
                       [this, group_id]() { DissolveGroupAfterConfirm(group_id); },
                       "../icons/trash.svg",
                       true,
                   });
                   ContextMenuHost::Instance().ShowActions(ui::Vector2i(120, 120), std::move(actions));
                   NotifySurfaceChanged();
                 });
    return;
  }
}

void ChatController::DissolveGroupAfterConfirm(const std::string& group_id) {
  ShowConfirm(Tr("chat.group.dissolve_title"), Tr("chat.group.dissolve_confirm"), [this, group_id](const bool ok) {
    if (!ok) {
      return;
    }
    // The chat stays for everyone, read-only: nothing is closed, the screen only refreshes.
    if (auto dissolved = presenter_.DissolveGroup(group_id); !dissolved) {
      UserFeedback::Fail(dissolved.error().message);
    }
    RefreshFromMessaging();
  }, true);
}

void ChatController::AfterThreadPrefsChanged() {
  RefreshFromMessaging();
  group_info_.Refresh(); // its mute button follows at once
  NotifySurfaceChanged();
}

void ChatController::OnSessionsSearchChanged() {
  ApplySessionList();
  DataModelHost::Instance().Dirty("shell", "sessions");
  DataModelHost::Instance().Dirty("shell", "sessions_more");
}

void ChatController::OnToggleArchivedAi() {
  presenter_.ToggleArchivedAi();
  RefreshFromMessaging();
  NotifySurfaceChanged();
}

void ChatController::OnToggleArchived() {
  session_window_.Reset();  // the other list starts at its first rows
  presenter_.ToggleArchived();
  RefreshFromMessaging();
  NotifySurfaceChanged();
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
    presenter_.ForgetCachedPrefs();  // clearing also resets the chat's "don't ask again" for the AI
    AfterHistoryCleared(thread_id);
  }, true);
}

void ChatController::AfterHistoryCleared(const std::string& thread_id) {
  ForgetComposerExtras(thread_id);
  // A thread cleared from the sessions list may not be the open one; only the open chat has state to reset.
  if (thread_id == ActiveThreadId()) {
    chat_.draft = "";
    chat_.status = "";
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
      }, true);
}

void ChatController::RefreshFromMessaging() {
  const int prev_sessions = ChromeSnapshot().nav_badges.sessions_unread;
  const int prev_contacts = ChromeSnapshot().nav_badges.contacts_unread;
  SyncShellSessions();
  SyncDisplayFromThread();
  UpdateThreadChrome(); // also refreshes the microphone button: show_voice follows messaging_ready_ and the chat
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
  mentions_.SyncThread();  // the open group's @ lines: reloaded when its members or names may have changed
  const NavBadgeState& badges = ChromeSnapshot().nav_badges;
  if (badges.sessions_unread != prev_sessions || badges.contacts_unread != prev_contacts) {
    ShellRemountNavRail();
  }
}

void ChatController::OnPeerIconsChanged() {
  if (!messaging_ready_) {
    return;
  }
  SyncShellSessions(); // chats list rows and Home's widgets are built from the presenter's list
  SyncDisplayFromThread();
  UpdateThreadChrome();
  group_info_.Refresh();
  DirtyChat();
  DirtyShell();
  NotifySurfaceChanged();
}

void ChatController::OnProfileDataReset() {
  CloseSearch(); // nothing decrypted from the old profile stays in memory
  messaging_ready_ = false;
  mesh_ready_ = false;
  // profile.db is recreated: nothing read from the old one may be written back into the new one.
  presenter_.ResetForNewProfile();
  drafts_ = ThreadDrafts{}; // unsent text of the old profile must not reach the new one
  quotes_.clear();
  mentions_.Reset();
  agent_.ForgetAllPendingImages();
  agent_.ClearForgottenThreads(); // the old profile's deleted conversations are not the new one's
  chat_.draft = "";
  DataModelHost::Instance().Dirty("chat", "draft");
  shell_.showing_archived = false;
  shell_.showing_archived_ai = false;
  session_window_.Reset();
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

namespace {
std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return s;
}

}  // namespace

// One chats-list row as the RML binds it. Both texts are bound as text ({{session.title}}), so names and message text
// need no escaping here.
SessionDisplayRow ToSessionDisplayRow(const ChatSessionRow& from) {
  SessionDisplayRow row;
  row.id = from.id.c_str();
  row.title = from.title_is_default_ai ? Tr("chat.new_chat").c_str() : from.title.c_str();
  row.preview = from.preview.c_str();
  row.avatar_letter = from.avatar_letter.c_str();
  row.avatar_tone = from.avatar_tone;
  row.has_icon = !from.icon_path.empty();
  row.icon_src = from.icon_path.c_str();
  row.kind = from.kind.c_str();
  row.unread_count = from.unread_count;
  row.unread_display = FormatBadgeCount(from.unread_count).c_str();
  row.date_label = from.date_label.c_str();
  row.active = from.active;
  row.pinned = from.pinned;
  row.muted = from.muted;
  row.unread_dot = from.unread_dot;
  row.mentioned = from.mentioned;
  row.blocked = from.blocked;
  if (from.blocked) {
    row.preview = Tr("sidebar.blocked_preview").c_str();
  }
  return row;
}

void ChatController::ApplySessionList() {
  const ChatListScreen& list = presenter_.List();
  shell_.sessions.clear();
  shell_.archived_count = list.archived_count;
  shell_.showing_archived = list.showing_archived;
  shell_.archived_entry_label =
      list.showing_archived ? Tr("sidebar.archived.back").c_str()
                            : Tr("sidebar.archived.entry", {{"count", std::to_string(list.archived_count)}}).c_str();
  shell_.archived_count_ai = list.archived_count_ai;
  shell_.showing_archived_ai = list.showing_archived_ai;
  shell_.archived_entry_label_ai =
      list.showing_archived_ai
          ? Tr("sidebar.archived.back_ai").c_str()
          : Tr("sidebar.archived.entry", {{"count", std::to_string(list.archived_count_ai)}}).c_str();
  // The open chat is always drawn, wherever it sorts (opened from Contacts, or pushed down by newer ones).
  for (size_t i = 0; i < list.rows.size(); ++i) {
    if (list.rows[i].active) {
      session_window_.Include(i);
      break;
    }
  }
  // A search shows every match, not just the loaded window.
  const std::string query = LowerAscii(shell_.sessions_query.c_str());
  const bool searching = !query.empty();
  const size_t visible = searching ? list.rows.size() : session_window_.Visible(list.rows.size());
  shell_.sessions_more = !searching && session_window_.More(list.rows.size());
  shell_.sessions.reserve(visible);
  for (size_t i = 0; i < visible; ++i) {
    const ChatSessionRow& from = list.rows[i];
    SessionRow row = ToSessionDisplayRow(from);
    if (searching && LowerAscii(row.title.c_str()).find(query) == std::string::npos &&
        LowerAscii(from.preview).find(query) == std::string::npos) {
      continue;
    }
    shell_.sessions.push_back(std::move(row));
  }
  // AI conversations for the AI 助手 page (no search window yet; the whole list, newest/pinned first).
  shell_.ai_sessions.clear();
  shell_.ai_sessions.reserve(list.ai_rows.size());
  for (const ChatSessionRow& from : list.ai_rows) {
    shell_.ai_sessions.push_back(ToSessionDisplayRow(from));
  }
  ApplyHomeToday(list);
}

void ChatController::ApplyHomeToday(const ChatListScreen& list) {
  std::tm now{};
  if (pp::civil_time::LocalTime(static_cast<std::time_t>(util::NowUnixMs() / 1000), &now)) {
    shell_.home_date_label = HomeDateLabel(now).c_str();
  }
  // The archived list replaces the rows while it shows: the Today widgets keep what the main list said.
  if (list.showing_archived) {
    return;
  }
  const HomeWidgetLayout layout = HomeLayout();
  // The newest mention, read or not, keeps the Mentions widget while it is recent (only read when it is kept).
  const std::optional<IThreadMentionsStore::Mention> latest_mention =
      layout.Kept(kHomeWidgetMentions) && messaging_ready_ && facade_ ? facade_->LatestMention() : std::nullopt;
  home_summary_ = SummarizeHomeToday(list.rows, layout, latest_mention, util::NowUnixMs());
  shell_.home_recent.clear();
  for (const ChatSessionRow& from : home_summary_.recent) {
    shell_.home_recent.push_back(ToSessionDisplayRow(from));
  }
  shell_.home_mentions = home_summary_.mentions.count;
  shell_.home_mentions_where =
      home_summary_.mentions.preview.empty()
          ? home_summary_.mentions.title.c_str()
          : (home_summary_.mentions.title + " · " + home_summary_.mentions.preview).c_str();
  shell_.home_new_people = home_summary_.new_contacts;
  shell_.home_security_ok = session_store_ && Store().Snapshot().profile_prefs.recovery_phrase_seen;
  shell_.home_show_dynamic = session_store_ && Store().Snapshot().profile_prefs.home_show_dynamic;
  if (layout.Kept(kHomeWidgetFavorites)) {
    ApplyHomeFavorites(list.rows);
  } else {
    home_favorites_.clear();
    shell_.home_favorites.clear();
  }
  ApplyHomeWidgets();
  RefreshHomeCalls(); // a missed call arrives as a chat change
}

void ChatController::ApplyHomeFavorites(const std::vector<ChatSessionRow>& rows) {
  const std::vector<std::string> starred = session_store_ ? Store().Snapshot().profile_prefs.starred_contacts
                                                          : std::vector<std::string>{};
  // A starred contact with no chat yet is looked up (the contacts store; a handful at most).
  std::vector<HomeFavorite> without_chat;
  if (facade_) {
    for (const std::string& id : StarredWithoutChat(rows, starred)) {
      auto contact = facade_->GetContact(id);
      if (!contact || !*contact || (*contact)->trust == TrustLevel::Blocked) {
        continue;
      }
      std::string title = ContactEffectiveTitle(**contact);
      if (title.empty()) {
        title = Tr("people_picker.unnamed");
      }
      const AvatarGlyph glyph = MakeAvatarGlyph(title, ContactAvatarStableId(**contact));
      without_chat.push_back({.contact_id = id,
                              .thread_id = {},
                              .title = title,
                              .avatar_letter = glyph.letter,
                              .avatar_tone = glyph.tone,
                              .icon_path = facade_->ContactIconPath(id)});
    }
  }
  home_favorites_ = HomeFavorites(rows, starred, without_chat);
  SyncHomeFavoriteSize();
  shell_.home_favorites.clear();
  for (const HomeFavorite& favorite : home_favorites_) {
    shell_.home_favorites.push_back({.contact_id = favorite.contact_id.c_str(),
                                     .title = favorite.title.c_str(),
                                     .avatar_letter = favorite.avatar_letter.c_str(),
                                     .avatar_tone = favorite.avatar_tone,
                                     .has_icon = !favorite.icon_path.empty(),
                                     .icon_src = favorite.icon_path.c_str(),
                                     .suggested = favorite.suggested});
  }
}

void ChatController::SyncHomeFavoriteSize() {
  if (!context_) {
    return;
  }
  const float ratio = context_->GetDensityIndependentPixelRatio();
  const float width_dp = static_cast<float>(context_->GetDimensions().x) / (ratio > 0.f ? ratio : 1.f);
  const int avatar = FavoritesRowModel::AvatarDp(width_dp);
  const ui::String size = (std::to_string(avatar) + "dp").c_str();
  if (size == shell_.home_fav_avatar) {
    return;
  }
  shell_.home_fav_avatar = size;
  shell_.home_fav_avatar_radius = (std::to_string((avatar + 1) / 2) + "dp").c_str();
  DataModelHost::Instance().Dirty("shell", "home_fav_avatar");
  DataModelHost::Instance().Dirty("shell", "home_fav_avatar_radius");
}

void ChatController::ApplyHomeWidgets() {
  const int64_t now_ms = util::NowUnixMs();
  const HomeWidgetLayout layout = HomeLayout();
  const std::vector<HomeBriefItem> top =
      layout.Kept(kHomeWidgetBrief) ? HomeBriefTop(home_brief_, HomeBriefFeed()) : std::vector<HomeBriefItem>{};
  shell_.home_brief.clear();
  for (const HomeBriefItem& item : top) {
    shell_.home_brief.push_back({.title = item.title.c_str(), .meta = HomeBriefMeta(item, now_ms).c_str()});
  }
  const HomeCalls calls = layout.Kept(kHomeWidgetCalls)
                              ? SummarizeHomeCalls(home_calls_, LocalMidnightMs(now_ms), now_ms)
                              : HomeCalls{};
  shell_.home_missed_calls = calls.missed_today;
  shell_.home_calls_latest = "";
  if (calls.latest) {
    // The chat's name as the Chats list shows it (the DM or the group); none when the chat is gone.
    std::string name;
    for (const SessionDisplayRow& row : shell_.sessions) {
      if (row.id.c_str() == calls.latest->thread_id) {
        name = row.title.c_str();
        break;
      }
    }
    std::string line = Tr(calls.latest->video ? "home.calls.video" : "home.calls.voice");
    if (!name.empty()) {
      line = name + " · " + line;
    }
    line += " · " + SessionDateLabel(calls.latest->at_ms, now_ms);
    shell_.home_calls_latest = Tr("home.calls.latest", {{"call", line}}).c_str();
  }
  shell_.home_widgets.clear();
  for (const std::string& id : ShownHomeWidgets(layout, HomeWidgetDataOf(home_summary_, calls, top.size()))) {
    shell_.home_widgets.push_back({.id = id.c_str(), .wide = HomeWidgetWide(id)});
  }
  ApplyHomeEditRows(home_edit_drag_ ? *home_edit_drag_ : layout);
}

void ChatController::ApplyHomeEditRows(const HomeWidgetLayout& layout) {
  shell_.home_edit_on.clear();
  shell_.home_edit_off.clear();
  for (const std::string& id : layout.order) {
    HomeEditRow row{.id = id.c_str(),
                    .name = Tr("home.widget." + id).c_str(),
                    .size = Tr(HomeWidgetWide(id) ? "home_edit.wide" : "home_edit.small").c_str()};
    (layout.Kept(id) ? shell_.home_edit_on : shell_.home_edit_off).push_back(std::move(row));
  }
  shell_.home_edit_dragging = home_edit_drag_ ? home_edit_drag_id_.c_str() : "";
}

namespace {

// Edit Home's drag, like iOS (Kenneth 2026-11-04): the held row is lifted (a little larger, above the others, with a
// shadow) and follows the finger; a row it passes slides into its new slot; on release it settles into its own.
// Everything moves on `transform` (layout and the slot maths stay put) and only while a row is held or settling.
constexpr float kHomeEditLift = 1.03f;
constexpr float kHomeEditSlideSeconds = 0.2f;
constexpr float kHomeEditFollowSeconds = 0.05f;

ui::Property HomeEditRowTransform(const float dy_px, const float scale) {
  return ui::Transform::MakeProperty({ui::Transforms::TranslateY(dy_px, ui::Unit::PX), ui::Transforms::Scale2D(scale)});
}

const ui::Tween kHomeEditEase(ui::Tween::Cubic, ui::Tween::Out);

/** A row's slot centre on the page, px (transforms do not move the layout box). */
float HomeEditSlotCentre(ui::Element* row) {
  return row->GetAbsoluteOffset(ui::BoxArea::Border).y + row->GetBox().GetSize(ui::BoxArea::Border).y * 0.5f;
}

/** The rows on Home in `card`, in order: the list's hidden template row (data-for) is left out. */
ui::ElementList HomeEditRows(ui::Element* card) {
  ui::ElementList all;
  card->QuerySelectorAll(all, ".home-edit-row");
  ui::ElementList rows;
  for (ui::Element* row : all) {
    if (row->IsVisible()) {
      rows.push_back(row);
    }
  }
  return rows;
}

} // namespace

void ChatController::OnHomeEditDragStart(const std::string& id) {
  const HomeWidgetLayout layout = HomeLayout();
  if (!layout.Kept(id)) {
    return;
  }
  home_edit_drag_ = layout;
  home_edit_drag_id_ = id;
  home_edit_grabbed_ = false;
  ApplyHomeEditRows(*home_edit_drag_);
  DirtyShell();
}

void ChatController::OnHomeEditDrag(ui::Event& event) {
  if (!home_edit_drag_) {
    return;
  }
  // The row whose centre is nearest the finger (the list of rows on Home, in the card the handle is in).
  ui::Element* card = event.GetCurrentElement();
  while (card && !card->IsClassSet("home-edit-card")) {
    card = card->GetParentNode();
  }
  if (!card) {
    return;
  }
  home_edit_card_ = card->GetObserverPtr();
  const float y = static_cast<float>(event.GetParameter<int>("mouse_y", 0));
  const ui::ElementList rows = HomeEditRows(card);
  const std::vector<std::string> on_home = home_edit_drag_->OnHome();
  const auto at = std::find(on_home.begin(), on_home.end(), home_edit_drag_id_);
  if (rows.empty() || at == on_home.end() || static_cast<size_t>(at - on_home.begin()) >= rows.size()) {
    return;
  }
  size_t current = static_cast<size_t>(at - on_home.begin());
  if (!home_edit_grabbed_) {
    // Where on the row the finger is (a handle is on the row, so at most half its height from the centre).
    const float half = rows[current]->GetBox().GetSize(ui::BoxArea::Border).y * 0.5f;
    home_edit_grab_px_ = std::clamp(y - HomeEditSlotCentre(rows[current]), -half, half);
    home_edit_grabbed_ = true;
  }
  // The row's centre where the finger holds it, kept between the first and last slots.
  const float first = HomeEditSlotCentre(rows.front());
  const float last = HomeEditSlotCentre(rows.back());
  const float held = std::clamp(y - home_edit_grab_px_, first, last);
  size_t nearest = current;
  float nearest_distance = std::abs(HomeEditSlotCentre(rows[current]) - held);
  for (size_t i = 0; i < rows.size(); ++i) {
    const float distance = std::abs(HomeEditSlotCentre(rows[i]) - held);
    if (distance < nearest_distance) {
      nearest = i;
      nearest_distance = distance;
    }
  }
  if (nearest != current) {
    home_edit_drag_ = MoveHomeWidget(*home_edit_drag_, home_edit_drag_id_, nearest);
    ApplyHomeEditRows(*home_edit_drag_);
    DirtyShell();
    // The list keeps its elements by position: each row between the old and new slot now shows the widget that was
    // one slot over, so it starts where that widget was drawn and slides home.
    const bool down = nearest > current;
    const size_t lo = down ? current : nearest + 1;
    const size_t hi = down ? nearest : current + 1;
    for (size_t i = lo; i < hi; ++i) {
      const size_t was = down ? i + 1 : i - 1;
      const ui::Property from = HomeEditRowTransform(HomeEditSlotCentre(rows[was]) - HomeEditSlotCentre(rows[i]), 1.f);
      rows[i]->Animate("transform", HomeEditRowTransform(0.f, 1.f), kHomeEditSlideSeconds, kHomeEditEase, 1, false,
                       0.f, &from);
    }
    current = nearest;
  }
  // The held row follows the finger (a short ease smooths the touch samples).
  rows[current]->Animate("transform", HomeEditRowTransform(held - HomeEditSlotCentre(rows[current]), kHomeEditLift),
                         kHomeEditFollowSeconds, kHomeEditEase);
}

void ChatController::OnHomeEditDragEnd() {
  if (!home_edit_drag_) {
    return;
  }
  const HomeWidgetLayout dragged = *home_edit_drag_;
  // The held row settles into its slot and comes back to its size.
  if (ui::Element* card = home_edit_card_.get(); card && home_edit_grabbed_) {
    const ui::ElementList rows = HomeEditRows(card);
    const std::vector<std::string> on_home = dragged.OnHome();
    const auto at = std::find(on_home.begin(), on_home.end(), home_edit_drag_id_);
    if (at != on_home.end() && static_cast<size_t>(at - on_home.begin()) < rows.size()) {
      rows[static_cast<size_t>(at - on_home.begin())]->Animate("transform", HomeEditRowTransform(0.f, 1.f),
                                                              kHomeEditSlideSeconds, kHomeEditEase);
    }
  }
  home_edit_card_ = nullptr;
  home_edit_grabbed_ = false;
  home_edit_drag_.reset();
  home_edit_drag_id_.clear();
  SaveHomeLayout(dragged);
  ApplyHomeWidgets(); // the lifted row comes down even when the order did not change
  DirtyShell();
}

void ChatController::RefreshHomeCalls() {
  // The widgets exist only on phones and are read while Home shows; a removed Calls widget is not read.
  if (home_calls_reading_) {
    home_calls_again_ = true; // a call may have been stored after that read began
    return;
  }
  if (ChromeSnapshot().layout_mode != LayoutMode::Compact ||
      ChromeSnapshot().nav_tab != NavTab::Home || !HomeLayout().Kept(kHomeWidgetCalls) ||
      !call_actions_.recent_calls_reader) {
    return;
  }
  std::function<std::vector<call_queries::RecentCall>()> read = call_actions_.recent_calls_reader(kHomeCallsRead);
  if (!read) {
    return;
  }
  home_calls_reading_ = true;
  AppRuntime::PostWorkerAndReplyOnUI<std::vector<call_queries::RecentCall>>(
      WorkerLane::Background, [read = std::move(read)]() { return read(); },
      [this](std::vector<call_queries::RecentCall> calls) {
        home_calls_reading_ = false;
        if (calls != home_calls_) {
          home_calls_ = std::move(calls);
          ApplyHomeWidgets();
          DirtyShell();
        }
        if (std::exchange(home_calls_again_, false)) {
          RefreshHomeCalls();
        }
      });
}

void ChatController::OnHomePrefsChanged() {
  ApplyHomeToday(presenter_.List());
  DirtyShell();
}

HomeWidgetLayout ChatController::HomeLayout() const {
  if (!session_store_) {
    return HomeWidgetLayoutFrom({}, {});
  }
  const ProfilePreferences& prefs = Store().Snapshot().profile_prefs;
  return HomeWidgetLayoutFrom(prefs.home_widgets_order, prefs.home_widgets_hidden);
}

void ChatController::OpenHomeEdit() {
  if (home_edit_layer_ >= 0 || !shell_navigation_.push_layer) {
    return;
  }
  ApplyHomeWidgets();
  DirtyShell();
  PaneSpec spec;
  spec.key = "home_edit";
  spec.page_sheet = true;
  home_edit_layer_ = shell_navigation_.push_layer(spec);
  if (home_edit_layer_ >= 0 && flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(home_edit_layer_, {}, [this]() { home_edit_layer_ = -1; });
  }
}

void ChatController::SaveHomeLayout(const HomeWidgetLayout& layout) {
  if (!session_store_ || layout == HomeLayout()) {
    return;
  }
  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  prefs.home_widgets_hidden = HiddenHomeWidgets(layout);
  prefs.home_widgets_order = layout.order;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
    log().warning << "Edit Home: could not save the choice: " << saved.error().message;
    ShowToast(UserFeedback::FailFrom(saved.error()));
    return;
  }
  // A widget put back is computed again; Calls and Today's briefs read / fetch when they need to.
  ApplyHomeToday(presenter_.List());
  DirtyShell();
  RefreshHomeBrief();
}

std::string ChatController::HomeBriefFeed() {
  return ArticleFeedName(LocalizationService::Instance().ResolvedLanguage());
}

void ChatController::RefreshHomeBrief() {
  // The widgets exist only on phones; the desktop's Home is the centred composer.
  if (!facade_ || ChromeSnapshot().layout_mode != LayoutMode::Compact) {
    return;
  }
  const std::string profile_dir = facade_->ProfileDataDir();
  if (profile_dir.empty()) {
    return;
  }
  if (profile_dir != home_brief_dir_) {
    home_brief_dir_ = profile_dir;
    home_brief_ = LoadHomeBriefCache(profile_dir);
    home_brief_failed_ms_ = 0;
    ApplyHomeWidgets();
    DirtyShell();
  }
  const std::string feed = HomeBriefFeed();
  const int64_t now_ms = util::NowUnixMs();
  // The demo app has no network: DemoDataSeeder's made-up headlines stand in for the feed.
  if constexpr (kUiDemoBuild) {
    if (home_brief_.feed != feed) {
      // As the feed would send them, so the items go through the same reading as a real answer.
      std::vector<Value> articles;
      for (const DemoHeadline& line : DemoBriefHeadlines(feed == "cn")) {
        Object article;
        article.set("title", std::string(line.title));
        article.set("link_to", std::string(line.url));
        article.set("created_at", now_ms - int64_t{line.minutes_ago} * 60 * 1000);
        articles.push_back(ObjectValue(std::move(article)));
      }
      Object doc;
      doc.set("articles", ArrayValue(std::move(articles)));
      home_brief_ = {.fetched_at_ms = now_ms,
                     .feed = feed,
                     .items = ParseHomeBriefArticles(DumpJson(doc)).value_or(std::vector<HomeBriefItem>{})};
      ApplyHomeWidgets();
      DirtyShell();
    }
    return;
  }
  // A widget the user removed is not fetched.
  if (!ShouldFetchHomeBrief(HomeLayout().Kept(kHomeWidgetBrief), home_brief_fetching_, home_brief_failed_ms_,
                            home_brief_, feed, now_ms)) {
    return;
  }
  home_brief_fetching_ = true;
  const AppConfig config = Store().Snapshot().config;
  const Object arguments = ArticleFeedArguments(LocalizationService::Instance().ResolvedLanguage());
  // The worker's answer: the new cache, or why there is none.
  using Fetched = std::pair<std::optional<HomeBriefCache>, std::string>;
  AppRuntime::PostWorkerAndReplyOnUI<Fetched>(
      WorkerLane::Background,
      [config, arguments, feed, profile_dir]() -> Fetched {
        auto answer = FetchArticleFeed(config, arguments);
        if (!answer) {
          return {std::nullopt, answer.error().message};
        }
        auto items = ParseHomeBriefArticles(*answer);
        if (!items) {
          return {std::nullopt, "the feed answer is not an article list"};
        }
        HomeBriefCache cache;
        cache.fetched_at_ms = util::NowUnixMs();
        cache.feed = feed;
        cache.items = std::move(*items);
        std::string note;
        if (auto saved = SaveHomeBriefCache(profile_dir, cache); !saved) {
          note = "cache not saved: " + saved.error().message;
        }
        return {std::move(cache), std::move(note)};
      },
      [this, profile_dir](Fetched fetched) {
        home_brief_fetching_ = false;
        if (!fetched.second.empty()) {
          log().info << "Home briefs: " << fetched.second;
        }
        if (profile_dir != home_brief_dir_) {
          return; // another profile is open now
        }
        if (!fetched.first) {
          home_brief_failed_ms_ = util::NowUnixMs();
          return;
        }
        home_brief_failed_ms_ = 0;
        home_brief_ = std::move(*fetched.first);
        ApplyHomeWidgets();
        DirtyShell();
      });
}

void ChatController::OpenHomeBrief(const int index) {
  const std::vector<HomeBriefItem> top = HomeBriefTop(home_brief_, HomeBriefFeed());
  if (index < 0 || static_cast<size_t>(index) >= top.size()) {
    return;
  }
  if (!top[static_cast<size_t>(index)].url.empty()) {
    OpenTrustedUrl(top[static_cast<size_t>(index)].url); // our own brief feed
    return;
  }
  SendSuggestionAction("articles");
}

void ChatController::OpenHomeNewPeople() {
  if (!home_summary_.first_stranger_thread_id.empty()) {
    OnSelectThread(home_summary_.first_stranger_thread_id);
  }
}

void ChatController::OpenHomeFavorite(const int index) {
  if (index < 0 || static_cast<size_t>(index) >= home_favorites_.size()) {
    return;
  }
  const HomeFavorite& favorite = home_favorites_[static_cast<size_t>(index)];
  if (!favorite.thread_id.empty()) {
    OnSelectThread(favorite.thread_id);
  } else if (contacts_notify_.message_contact) {
    contacts_notify_.message_contact(favorite.contact_id);
  }
}

void ChatController::OpenHomeCalls() {
  const HomeCalls calls = SummarizeHomeCalls(home_calls_, LocalMidnightMs(util::NowUnixMs()), util::NowUnixMs());
  if (calls.latest && !calls.latest->thread_id.empty()) {
    OnSelectThread(calls.latest->thread_id);
  }
}

void ChatController::OpenHomeMentions() {
  if (!home_summary_.mentions.thread_id.empty()) {
    OnSelectThread(home_summary_.mentions.thread_id); // opens at its oldest unread mention (G015)
  }
}

void ChatController::OnMessagesScroll() {
  scroller_.OnMessagesScroll();
}

void ChatController::OnSessionsScroll() {
  ui::Element* el = SessionListElement();
  if (!el) {
    return;
  }
  const bool near_bottom = SessionListNearBottom(el->GetScrollTop(), el->GetScrollHeight(), el->GetClientHeight());
  if (session_window_.ExtendIfNearBottom(presenter_.List().rows.size(), near_bottom)) {
    ApplySessionList();
    DirtyShell();
    NotifySurfaceChanged();
  }
}

void ChatController::OnSessionsLoadMore() {
  if (session_window_.Extend(presenter_.List().rows.size())) {
    ApplySessionList();
    DirtyShell();
    NotifySurfaceChanged();
  }
}

void ChatController::FillSessionListAfterLayout() {
  // Rows that do not reach the bottom of the list cannot be scrolled, so the scroll event never asks for more:
  // after a layout, grow the window until the list overflows (or every row is drawn). One step per layout.
  if (!shell_.sessions_more) {
    return;
  }
  ui::Element* el = SessionListElement();
  if (!el || !SessionListNeedsFill(el->GetScrollHeight(), el->GetClientHeight())) {
    return;
  }
  OnSessionsLoadMore();
}

ui::Element* ChatController::SessionListElement() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context_->GetDocument(0)->GetElementById("sidebar-sessions");
}

void ChatController::OnJumpToLatest() {
  scroller_.OnJumpToLatest();
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
  RefreshPendingAttachmentChip(/*force=*/false);
  if (const std::string thread_id = ActiveThreadId(); thread_id != voice_thread_) {
    // Another chat is open: a recording in the one before can no longer be followed, and a clip is not its bubble's.
    voice_thread_ = thread_id;
    voice_->Lost();
    voice_playback_->Stop();
  }
  voice_->Refresh();
  SyncComposerInputState();
}

void ChatController::RefreshShowCapture() {
  const bool show =
      chat_.show_attach_button &&
      ShowCaptureEntry(Platform::IsMobile(), facade_ && facade_->AnyCallLive(), InAiComposerContext(),
                       chat_.attachment_uploading || chat_.attachment_preparing || chat_.attachment_failed);
  if (chat_.show_capture != show) {
    chat_.show_capture = show;
    DataModelHost::Instance().Dirty("chat", "show_capture");
  }
}

void ChatController::ToggleVoiceMode() {
  if (chat_.voice_recording || (!chat_.voice_mode && !chat_.show_voice)) {
    return;
  }
  chat_.voice_mode = !chat_.voice_mode;
  if (chat_.voice_mode && context_ && context_->GetNumDocuments() > 0) {
    // The text input is going away: let the keyboard go with it.
    if (ui::Element* draft = context_->GetDocument(0)->GetElementById("draft-input")) {
      draft->Blur();
    }
  }
  DataModelHost::Instance().Dirty("chat", "voice_mode");
}

void ChatController::BeginAttachmentPreparing(const std::string& name) {
  if (chat_.attachment_uploading || !preparing_.Begin(std::chrono::steady_clock::now())) {
    return;
  }
  SyncAttachmentPreparing();
  chat_.attachment_draft_name = name.c_str(); // empty when the platform did not say
  DataModelHost::Instance().Dirty("chat", "attachment_draft_name");
}

void ChatController::EndAttachmentPreparing() {
  preparing_.End();
  SyncAttachmentPreparing();
}

void ChatController::SyncAttachmentPreparing() {
  if (chat_.attachment_preparing == preparing_.Busy()) {
    return;
  }
  chat_.attachment_preparing = preparing_.Busy();
  // Background time for the copy, so a quick switch to another app does not stall it.
  if (chat_.attachment_preparing) {
    preparing_activity_ = std::make_unique<BackgroundActivity>("attachment-prepare");
  } else {
    preparing_activity_.reset();
  }
  if (chat_.attachment_preparing) {
    attachment_thread_ = ActiveThreadId();
    chat_.attachment_failed = false;
    chat_.attachment_draft_name = "";
    chat_.attachment_status = Tr("chat.attachment.sending").c_str();
  } else {
    chat_.attachment_status = "";
  }
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

void ChatController::RefreshAttachmentHere() {
  const bool here =
      AttachmentChipHere(chat_.attachment_uploading || chat_.attachment_preparing || chat_.attachment_failed,
                         attachment_thread_, ActiveThreadId());
  if (chat_.attachment_here != here) {
    chat_.attachment_here = here;
    DataModelHost::Instance().Dirty("chat", "attachment_here");
  }
}

void ChatController::SyncComposerInputState() {
  RefreshAttachmentHere();
  // The chip and the locked input belong to the chat the file is being sent in; the other chats stay typeable.
  chat_.composer_input_disabled =
      chat_.compose_disabled || chat_.attachment_here || chat_.image_preparing || chat_.voice_recording;
  // Whether a voice message can be sent depends on the same upload state: a toggle that went away while a file
  // was being sent must come back when the upload ends, not at the next chrome update.
  if (voice_) {
    voice_->Refresh();
  }
  RefreshShowCapture();
}

void ChatController::OnAttachFile(ui::Event* ev) {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  if (chat_.composer_input_disabled || chat_.attachment_uploading || chat_.attachment_preparing ||
      !chat_.show_attach_button) {
    return;
  }
  // B90: the menu opens at the attach button, not at a fixed corner (which was behind the keyboard on a phone).
  if (ev) {
    const ui::Vector2i anchor = MenuPositionAboveEvent(*ev);
    attach_menu_x_ = anchor.x;
    attach_menu_y_ = anchor.y;
  }
  if (InAiComposerContext()) {
    agent_.OnAttachAiImage();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  // Where the file came from decides what is done to a picture (see OutgoingPickSource).
  const auto picked = [this](const OutgoingPickSource source) {
    return [this, source](std::vector<std::string> paths) {
      AppRuntime::PostUI([this, source, paths = std::move(paths)]() mutable {
        EndAttachmentPreparing();
        if (paths.empty()) {
          return;
        }
        StartAttachmentUpload(std::move(paths.front()), source);
      });
    };
  };
  // A picture is asked about before it goes: sent smaller (the default) or as it is. A video from the phone's
  // library has no such choice and goes straight on.
  const auto picked_picture = [this]() {
    return [this](std::vector<std::string> paths) {
      AppRuntime::PostUI([this, paths = std::move(paths)]() mutable {
        EndAttachmentPreparing();
        if (paths.empty()) {
          return;
        }
        std::string path = std::move(paths.front());
        if (AttachmentMimeFromFilename(path).rfind("video/", 0) == 0) {
          StartAttachmentUpload(std::move(path), OutgoingPickSource::Photo);
          return;
        }
        ShowPictureSendChoice(std::move(path));
      });
    };
  };
  // "Photo" on a phone: the library with pictures and videos.
  const auto pick_photo_or_video = [this, picked_picture]() {
    return [this, on_picked = picked_picture()]() {
      ShowOpenPhotoLibraryDialog(Backend::GetWindow(), on_picked, [this](const std::string& name) {
        AppRuntime::PostUI([this, name]() { BeginAttachmentPreparing(name); });
      });
    };
  };
  // "Picture" on the desktop: the image-file dialog.
  const auto pick_image = [picked_picture]() {
    return [on_picked = picked_picture()]() { ShowOpenImageFileDialog(Backend::GetWindow(), on_picked); };
  };
  const auto pick_file = [picked]() {
    return [on_picked = picked(OutgoingPickSource::File)]() { ShowOpenFileDialog(Backend::GetWindow(), on_picked); };
  };

  // A phone keeps photos and files apart; the desktop dialog shows both at once, so there a picture and a file
  // have their own entries.
  std::vector<ContextMenuAction> actions;
  if (Platform::IsMobile()) {
    actions.push_back({"attach_photo", Tr("chat.attach.photo"), nullptr, pick_photo_or_video(), "../icons/v2/photo.svg"});
  } else {
    actions.push_back({"attach_picture", Tr("chat.attach.picture"), nullptr, pick_image()});
  }
  actions.push_back({"attach_file", Tr("chat.attach.file"), nullptr, pick_file(), "../icons/v2/file.svg"});
  actions.push_back(AttachCancelAction());
  ContextMenuHost::Instance().ShowActionsAbove(ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions), {},
                                               Tr("chat.attach.title"));
}

void ChatController::ShowPictureSendChoice(std::string path) {
  // Only one of the three runs; Cancel releases the file (the two sends hand it to StartAttachmentUpload). A sheet
  // closed without choosing (tap beside it, Escape) releases it too, so no private copy outlives the choice.
  auto release = [this](const std::string& picture) { release_pick_(picture); };
  auto actions = PictureChoiceActions(
      path,
      [this](const std::string& picture, const bool original) {
        StartAttachmentUpload(picture, original ? OutgoingPickSource::Original : OutgoingPickSource::Photo);
      },
      release, Tr("chat.attach.send_compressed"), Tr("chat.attach.send_original"), Tr("common.cancel"));
  ContextMenuHost::Instance().ShowActionsAbove(ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions),
                                               [release, picture = std::move(path)]() { release(picture); },
                                               Tr("chat.attach.send_photo_title"));
}

void ChatController::OnAttachCapture(ui::Event* ev) {
  if (!messaging_ready_ || !facade_ || !chat_.show_capture || chat_.composer_input_disabled ||
      chat_.attachment_uploading || chat_.attachment_preparing || InAiComposerContext() || ActiveThreadId().empty()) {
    return;
  }
  if (ev) {
    const ui::Vector2i anchor = MenuPositionAboveEvent(*ev);
    attach_menu_x_ = anchor.x;
    attach_menu_y_ = anchor.y;
  }
  if (Platform::Detect() != PlatformKind::Android) {
    OpenCameraCapture(CameraCaptureKind::PhotoOrVideo); // the iPhone camera switches photo / video itself
    return;
  }
  // Android has one system camera intent per kind: ask which.
  std::vector<ContextMenuAction> actions;
  actions.push_back({"capture_photo", Tr("chat.attach.capture_photo"), nullptr,
                     [this]() { OpenCameraCapture(CameraCaptureKind::Photo); }, "../icons/v2/camera.svg"});
  actions.push_back({"capture_video", Tr("chat.attach.capture_video"), nullptr,
                     [this]() { OpenCameraCapture(CameraCaptureKind::Video); }, "../icons/v2/video.svg"});
  actions.push_back(AttachCancelAction());
  ContextMenuHost::Instance().ShowActionsAbove(ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions), {},
                                               Tr("chat.attach.capture"));
}

void ChatController::OpenCameraCapture(const CameraCaptureKind kind) {
  if (!facade_) {
    return;
  }
  const AttachmentLimits limits = facade_->CurrentAttachmentLimits();
  ShowCameraCapture(
      Backend::GetWindow(), kind, limits.video_max_seconds, limits.max_video_bytes,
      [this](const CameraCaptureOutcome outcome, std::vector<std::string> paths) {
        AppRuntime::PostUI([this, outcome, paths = std::move(paths)]() mutable {
          EndAttachmentPreparing();
          HandleCaptureResult(MakeCaptureFlowPorts(), outcome, std::move(paths));
        });
      },
      [this](const std::string& name) { AppRuntime::PostUI([this, name]() { BeginAttachmentPreparing(name); }); });
}

CaptureFlowPorts ChatController::MakeCaptureFlowPorts() {
  CaptureFlowPorts ports;
  ports.release = [this](const std::string& path) { release_pick_(path); };
  ports.ask_picture = [this](std::string path) { ShowPictureSendChoice(std::move(path)); };
  ports.send_video = [this](std::string path) { StartAttachmentUpload(path, OutgoingPickSource::Photo); };
  ports.say = [this](const CaptureNotice notice) {
    switch (notice) {
    case CaptureNotice::CameraUnavailable:
      UserFeedback::Fail(Tr("chat.attach.camera_unavailable"));
      break;
    case CaptureNotice::VideoUnprocessable:
      UserFeedback::Fail(Tr("chat.attach.video_unprocessable"));
      break;
    case CaptureNotice::VideoTooLarge:
      UserFeedback::Fail(Tr("chat.attachment.video_too_large",
                            {{"limit", FormatAttachmentLimit(facade_->CurrentAttachmentLimits().max_video_bytes)}}));
      break;
    }
  };
  ports.file_size = [](const std::string& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? uint64_t{0} : static_cast<uint64_t>(size);
  };
  ports.limits = [this]() { return facade_ ? facade_->CurrentAttachmentLimits() : AttachmentLimits{}; };
  return ports;
}

std::function<bool(uint64_t done, uint64_t total)> ChatController::MakeSendProgress() {
  const uint64_t stay_hint_bytes = facade_ ? facade_->CurrentAttachmentLimits().stay_hint_bytes : 0;
  auto last_percent = std::make_shared<std::atomic<int>>(-1);
  return [this, last_percent, stay_hint_bytes](const uint64_t done, const uint64_t total) {
    if (total == 0) {
      return true;
    }
    const int percent = static_cast<int>(std::min<uint64_t>(100, done * 100 / total));
    if (last_percent->exchange(percent) == percent) {
      return true; // the line changes only when the number does
    }
    const bool large = total >= stay_hint_bytes;
    AppRuntime::PostUI([this, percent, large]() {
      if (!chat_.attachment_uploading) {
        return;
      }
      chat_.attachment_status =
          Tr(large ? "chat.attachment.uploading_stay" : "chat.attachment.uploading_percent",
             {{"percent", std::to_string(percent)}})
              .c_str();
      chat_.attachment_progress_width = (std::to_string(percent) + "%").c_str();
      DataModelHost::Instance().Dirty("chat", "attachment_status");
      DataModelHost::Instance().Dirty("chat", "attachment_progress_width");
    });
    return true;
  };
}

void ChatController::ShowFailedAttachment(const std::string& record_id, const std::string& name) {
  failed_record_id_ = record_id;
  chat_.attachment_failed = true;
  chat_.attachment_uploading = false;
  attachment_thread_ = ActiveThreadId(); // every caller shows the open chat's pending file
  chat_.attachment_draft_name = name.c_str();
  chat_.attachment_status = Tr("chat.attachment.send_failed").c_str();
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
}

void ChatController::RefreshPendingAttachmentChip(const bool force) {
  if (!facade_ || !messaging_ready_ || chat_.attachment_uploading || chat_.attachment_preparing) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (!force && thread_id == pending_chip_thread_) {
    return;
  }
  pending_chip_thread_ = thread_id;
  failed_record_id_.clear();
  if (!failed_voice_path_.empty() && thread_id == failed_voice_thread_) {
    ShowFailedAttachment(std::string(), std::filesystem::path(failed_voice_path_).filename().string());
    return; // a voice recording that was not staged: its own Retry / Discard
  }
  const bool had_chip = chat_.attachment_failed;
  chat_.attachment_failed = false;
  if (!thread_id.empty() && !InAiComposerContext()) {
    const auto pending = facade_->PendingOutgoingAttachments(thread_id);
    if (!pending.empty()) {
      ShowFailedAttachment(pending.front().id, pending.front().fields.filename);
      return;
    }
  }
  if (had_chip) {
    chat_.attachment_draft_name = "";
    chat_.attachment_status = "";
    SyncComposerInputState();
    DirtyChatChrome();
    DirtyChatHeader();
  }
}

void ChatController::KeepVoiceForRetry(const std::string& path, const std::string& thread_id) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::exists(path, ec)) {
    return; // already gone (the recordings are wiped at start and on lock): nothing to retry
  }
  failed_voice_path_ = path;
  failed_voice_thread_ = thread_id;
  failed_record_id_.clear();
  if (thread_id == ActiveThreadId()) {
    ShowFailedAttachment(std::string(), std::filesystem::path(path).filename().string());
  }
}

void ChatController::OnDiscardAttachmentSend() {
  if (!failed_voice_path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(failed_voice_path_, ec);
    failed_voice_path_.clear();
    failed_voice_thread_.clear();
    chat_.attachment_failed = false;
    chat_.attachment_draft_name = "";
    chat_.attachment_status = "";
    pending_chip_thread_.clear();
    RefreshPendingAttachmentChip(/*force=*/true);
    SyncComposerInputState();
    DirtyChatChrome();
    DirtyChatHeader();
    return;
  }
  if (!facade_ || failed_record_id_.empty()) {
    return;
  }
  facade_->DiscardOutgoingAttachment(failed_record_id_);
  RefreshPendingAttachmentChip(/*force=*/true);
}

void ChatController::OnCancelAttachmentSend() {
  if (!chat_.attachment_uploading || !send_cancel_ || send_cancel_->exchange(true)) {
    return;
  }
  // The card goes when the send stops (the upload at its next step; a direct push when it answers).
  chat_.attachment_status = Tr("chat.attachment.cancelling").c_str();
  chat_.attachment_cancelling = true;
  DataModelHost::Instance().Dirty("chat", "attachment_status");
  DataModelHost::Instance().Dirty("chat", "attachment_cancelling");
}

void ChatController::ResetUploadCard(const std::string& name) {
  if (!chat_.attachment_thumb.empty()) {
    std::error_code ec;
    std::filesystem::remove(std::string(chat_.attachment_thumb.c_str()), ec); // a copy in "picked": ours to delete
  }
  chat_.attachment_thumb = "";
  chat_.attachment_progress_width = "0%";
  chat_.attachment_type_label = AttachmentTypeLabel(name, AttachmentMimeFromFilename(name)).c_str();
  chat_.attachment_cancelling = false;
}

void ChatController::OnRetryAttachmentSend() {
  if (!failed_voice_path_.empty()) {
    if (facade_ && !chat_.attachment_uploading) {
      const std::string path = failed_voice_path_;
      const std::string thread_id = failed_voice_thread_;
      StartAttachmentUpload(path, OutgoingPickSource::File, /*voice_file=*/true, thread_id);
    }
    return;
  }
  if (!facade_ || failed_record_id_.empty() || chat_.attachment_uploading) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  const std::string record_id = failed_record_id_;
  if (thread_id.empty()) {
    return;
  }
  chat_.attachment_failed = false;
  chat_.attachment_uploading = true;
  attachment_thread_ = thread_id;
  send_cancel_ = std::make_shared<std::atomic<bool>>(false);
  const auto cancel = send_cancel_;
  ResetUploadCard(chat_.attachment_draft_name.c_str());
  chat_.attachment_status = Tr("chat.attachment.uploading").c_str();
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
  // Same dialog as a first send when the space is full: free the oldest file, then send the staged ciphertext again.
  BlobQuotaRecoveryFlow::RunUploadAsync<ThreadMessage>(
      [this, thread_id, record_id, cancel](std::function<void(Roe<ThreadMessage>)> done) {
        facade_->ResumeAttachmentAsync(thread_id, record_id, std::move(done), MakeSendProgress(), cancel);
      },
      [this, cancel](Roe<ThreadMessage> sent) {
        chat_.attachment_uploading = false;
        ResetUploadCard(std::string());
        chat_.attachment_status = "";
        chat_.attachment_draft_name = "";
        failed_record_id_.clear();
        SyncComposerInputState();
        const bool stale_chip = !sent && sent.error().category == 0 &&
                                sent.error().code == OutgoingAttachmentStore::kWrongThread;
        if (!sent && !stale_chip && !cancel->load()) {
          UserFeedback::Fail(UserFeedback::UserMessage(sent.error()));
        } else if (sent && cancel->load()) {
          UserFeedback::Fail(Tr("chat.attachment.already_sent")); // the cancel came after it reached the contact
        }
        RefreshPendingAttachmentChip(/*force=*/true); // still staged: Retry again; gone or sent: nothing shown
        if (sent) {
          SyncDisplayFromThread();
          scroller_.RequestScrollToLatest();
          UpdateSidebarPreview(sent->text);
        }
        DirtyChatChrome();
        DirtyChatHeader();
      },
      [this](const Error& quota_error) { return facade_->PlanRelayQuotaRecovery(quota_error); },
      [this](const std::string& blob_id) { return facade_->DeleteRelayFileById(blob_id); });
}

void ChatController::StartAttachmentUpload(const std::string& path, const OutgoingPickSource source,
                                           const bool voice_file, const std::string& voice_thread_id) {
  if (!facade_ || chat_.attachment_uploading || chat_.attachment_preparing) {
    release_pick_(path);
    return;
  }
  // A voice recording goes to the chat it was recorded in, which may no longer be the open one.
  const std::string thread_id = voice_file && !voice_thread_id.empty() ? voice_thread_id : ActiveThreadId();
  if (thread_id.empty()) {
    release_pick_(path);
    return;
  }
  if (voice_file) {
    failed_voice_path_.clear(); // this attempt is the retry
    failed_voice_thread_.clear();
  }
  const size_t staged_before = voice_file ? facade_->PendingOutgoingAttachments(thread_id).size() : 0;

  chat_.attachment_uploading = true;
  chat_.attachment_failed = false;
  attachment_thread_ = thread_id;
  failed_record_id_.clear();
  send_cancel_ = std::make_shared<std::atomic<bool>>(false);
  const auto cancel = send_cancel_;
  const std::string shown_name = DisplayNameFromPickedPath(path);  // a content:// URI is not a name
  chat_.attachment_draft_name =
      (shown_name.empty() ? std::filesystem::path(path).filename().string() : shown_name).c_str();
  ResetUploadCard(chat_.attachment_draft_name.c_str());
  chat_.status = Tr("chat.attachment.uploading").c_str();
  chat_.attachment_status = Tr("chat.attachment.uploading").c_str();
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();

  const auto finish_uploading = [this]() {
    chat_.attachment_uploading = false;
    ResetUploadCard(std::string());
    chat_.attachment_draft_name = "";
    chat_.attachment_status = "";
    chat_.status = "";
    SyncComposerInputState();
    DirtyChatChrome();
    DirtyChatHeader();
  };

  // Reading, decoding and shrinking a photo is blocking work: off the UI thread, reply on it.
  const AttachmentLimits limits = facade_->CurrentAttachmentLimits();
  const std::string scratch_dir = (std::filesystem::path(facade_->ProfileDataDir()) / "picked").string();
  AppRuntime::PostWorkerNormal([this, thread_id, path, source, limits, scratch_dir, finish_uploading, voice_file,
                                staged_before, cancel]() {
    auto prepared = PrepareOutgoingAttachment(path, limits, source, scratch_dir);
    std::string thumb = prepared && !voice_file ? WriteUploadThumbnail(path, *prepared, scratch_dir) : std::string();
    release_pick_(path); // read by now (and kept as a CAS copy once sent)
    AppRuntime::PostUI([this, thread_id, path, finish_uploading, voice_file, staged_before, cancel, thumb,
                        prepared = std::move(prepared)]() mutable {
      if (chat_.attachment_uploading && send_cancel_ == cancel && prepared) {
        chat_.attachment_thumb = thumb.c_str(); // the card shows it; ResetUploadCard deletes it
        chat_.attachment_type_label = AttachmentTypeLabel(prepared->filename, prepared->mime).c_str();
        DataModelHost::Instance().Dirty("chat", "attachment_thumb");
        DataModelHost::Instance().Dirty("chat", "attachment_type_label");
      } else if (!thumb.empty()) {
        std::error_code ec;
        std::filesystem::remove(thumb, ec);
      }
      if (!prepared) {
        finish_uploading();
        UserFeedback::Fail(UserFeedback::UserMessage(prepared.error()));
        if (voice_file) {
          KeepVoiceForRetry(path, thread_id); // not read: the recording is the only copy
          RefreshPendingAttachmentChip(/*force=*/true);
        }
        return;
      }
      // The bytes go to the hub once (moved: no second whole-file copy here). If the space is full the dialog's
      // retry sends the staged ciphertext (resume), so nothing is kept here after the first attempt.
      auto attachment = std::make_shared<OutgoingAttachment>(std::move(*prepared));
      auto attempts = std::make_shared<int>(0);
      BlobQuotaRecoveryFlow::RunUploadAsync<ThreadMessage>(
          [this, thread_id, attachment, attempts, cancel](std::function<void(Roe<ThreadMessage>)> done) {
            if ((*attempts)++ == 0) {
              facade_->SendAttachmentAsync(thread_id, std::move(*attachment), std::move(done), MakeSendProgress(),
                                           cancel);
              return;
            }
            const auto staged = facade_->PendingOutgoingAttachments(thread_id);
            if (staged.empty()) {
              done(Error("The attachment is no longer staged").WithUser(Tr("chat.attachment.pick_again")));
              return;
            }
            facade_->ResumeAttachmentAsync(thread_id, staged.back().id, std::move(done), MakeSendProgress(), cancel);
          },
          [this, thread_id, path, finish_uploading, voice_file, staged_before, cancel](Roe<ThreadMessage> sent) {
            finish_uploading();
            const bool cancelled = !sent && cancel->load(); // nothing was sent and nothing is kept: no message
            if (voice_file && cancelled) {
              std::error_code ec;
              std::filesystem::remove(path, ec);
            } else if (voice_file) {
              // The recording's file goes once the message is out or its encrypted copy is staged for a retry; a send
              // refused before staging (the rate limit, for one) keeps it, with a Retry / Discard of its own.
              const size_t staged_after = facade_->PendingOutgoingAttachments(thread_id).size();
              if (DecideVoiceFileFate(static_cast<bool>(sent), staged_before, staged_after) == VoiceFileFate::Delete) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
              } else {
                KeepVoiceForRetry(path, thread_id);
              }
            }
            if (!sent) {
              if (!cancelled) {
                UserFeedback::Fail(UserFeedback::UserMessage(sent.error()));
              }
              RefreshPendingAttachmentChip(/*force=*/true); // a kept ciphertext shows Retry
              return;
            }
            if (cancel->load()) {
              UserFeedback::Fail(Tr("chat.attachment.already_sent")); // the cancel came after it reached the contact
            }
            SyncDisplayFromThread();
            if (thread_id == ActiveThreadId()) {
              scroller_.RequestScrollToLatest();
              UpdateSidebarPreview(sent->text);
            }
          },
          [this](const Error& quota_error) { return facade_->PlanRelayQuotaRecovery(quota_error); },
          [this](const std::string& blob_id) { return facade_->DeleteRelayFileById(blob_id); });
    });
  });
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
  actions.push_back({"attach_photo", Tr("chat.attach.photo"), nullptr, std::move(photo), "../icons/v2/photo.svg"});
  actions.push_back({"attach_file", Tr("chat.attach.file"), nullptr, std::move(file), "../icons/v2/file.svg"});
  actions.push_back(AttachCancelAction());
  ContextMenuHost::Instance().ShowActionsAbove(ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions), {},
                                               Tr("chat.attach.title"));
}


std::vector<MessageDisplayRow> ChatController::BuildMessageRows(const std::string& thread_id,
                                                                std::optional<int64_t> oldest_inclusive,
                                                                std::optional<int64_t> newest_inclusive) const {
  if (!facade_) {
    return {};
  }
  TranscriptFacts facts = facade_->TranscriptFactsFor();
  facts.voice_supported = VoiceMessagesSupported();
  facts.voice_max_seconds = facade_->CurrentAttachmentLimits().voice_max_seconds;
  facts.voice_state = [this](const std::string& message_id) { return voice_playback_->State(message_id); };
  facts.audio_files_play = voice_playback_->PlaysFiles();
  std::vector<MessageDisplayRow> rows =
      RenderTranscript(facade_->BuildTranscript(thread_id, oldest_inclusive, newest_inclusive), facts,
                       presenter_.MentionRosterOf(thread_id));
  agent_.DecorateAiImageRows(rows, thread_id);
  return rows;
}


void ChatController::OpenGroupSender(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  auto message = facade_->GetMessage(facade_->ActiveThreadId(), message_id);
  if (!message || !*message) {
    return;
  }
  // The sender is a contact id, or the account id of someone who is not a contact (as PeerDisplayResolver reads it);
  // our own and the assistant's rows have no avatar or name to tap.
  const std::string sender = (*message)->sender_contact_id;
  if (sender.empty() || sender == kLocalSelfContactId || sender == kAiAssistantContactId) {
    return;
  }
  auto contact = facade_->GetContact(sender);
  if (!contact || !*contact) {
    contact = facade_->FindContactByIdentity(sender, mesh::ContactIdKind::Account);
  }
  // Over the chat as a page sheet on phones (Kenneth 2026-11-04: swiped down, back in the chat); the desktop's pane.
  if (contact && *contact) {
    if (contacts_notify_.select_contact_sheet) {
      contacts_notify_.select_contact_sheet((*contact)->id);
    }
  } else if (contacts_notify_.show_member_sheet) {
    // Not a contact: the same page in its member state (name, photo, Account ID, Add contact).
    const TranscriptFacts facts = facade_->TranscriptFactsFor();
    contacts_notify_.show_member_sheet(sender, facts.sender_label ? facts.sender_label(sender) : std::string(),
                                       facts.sender_icon ? facts.sender_icon(sender) : std::string());
  }
  // The group's owner finds the "Group admin" card at the bottom, as from the member list.
  if (people_picker_notify_.offer_group_admin) {
    people_picker_notify_.offer_group_admin(facade_->ActiveThreadId(), sender);
  }
}

void ChatController::OpenPicture(const std::string& message_id, const std::string& local_path) {
  if (!messaging_ready_ || !facade_ || message_id.empty() || opening_message_ids_.count(message_id) != 0) {
    return;
  }
  // The picture the bubble shows, full screen with a spinner, in this frame; the viewer opens over it.
  if (!local_path.empty() && shell_feedback_.mount_photo_preview && photo_preview_.Begin(message_id)) {
    shell_feedback_.mount_photo_preview(PhotoPreviewRml(local_path));
  }
  OpenAttachment(message_id);
}

void ChatController::OpenAttachment(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  // Once: a second tap while the first is still making the file does nothing.
  if (!opening_message_ids_.insert(message_id).second) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  const auto tapped = std::chrono::steady_clock::now();
  // The lookup, the decrypt of a stored copy and the checks are blocking work: off the UI thread.
  AppRuntime::PostWorkerNormal([this, thread_id, message_id, tapped]() {
    const auto ms_since = [](const std::chrono::steady_clock::time_point from) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count();
    };
    const auto info = facade_->AttachmentInfoForMessage(thread_id, message_id);
    const int64_t lookup_ms = ms_since(tapped);
    const bool is_video = info && IsAttachmentVideoMime(info->mime);
    if (info && OpenShowsOpeningToast(/*picture_preview=*/false, info->byte_length)) {
      AppRuntime::PostUI([this, message_id]() {
        if (opening_message_ids_.count(message_id) != 0 && !photo_preview_.Up()) {
          UserFeedback::Ok(Tr("chat.attachment.opening")); // decrypting a large file takes a moment
        }
      });
    }
    bool made_view = false;
    auto path = facade_->AttachmentOpenPathForMessage(thread_id, message_id, &made_view);
    const bool needs_confirm = path && facade_->AttachmentOpenNeedsConfirmForMessage(thread_id, message_id);
    // The open path carries the sender's name, so its extension types a sound file sent as octet-stream too.
    const bool is_sound = info && path && IsAudioFileAttachment(info->mime, *path);
    const int64_t worker_ms = ms_since(tapped);
    AppRuntime::PostUI([this, thread_id, message_id, is_video, is_sound, made_view, tapped, lookup_ms, worker_ms,
                        path = std::move(path), needs_confirm]() {
      opening_message_ids_.erase(message_id);
      // Result in (ready or not): the preview goes, on the next frame so the viewer is already presenting.
      const bool clear_preview = photo_preview_.Finish(message_id);
      const auto dismiss_preview = [this, clear_preview]() {
        if (clear_preview && shell_feedback_.mount_photo_preview) {
          AppRuntime::PostUI([this]() { shell_feedback_.mount_photo_preview(std::string()); });
        }
      };
      if (!path) {
        dismiss_preview();
        UserFeedback::Fail(Tr("chat.attachment.not_ready"));
        return;
      }
      if (made_view) {
        SyncDisplayFromThread(); // the card now has its view (a large video, its poster)
      }
      const auto open_file = [path = *path, is_video, dismiss_preview, needs_confirm]() {
        const bool opened = PlatformOpenFile(path);
        if (!needs_confirm) {
          dismiss_preview(); // a confirmed open already dismissed it before the question
        }
        if (!opened) {
          // A video this device has no player for (an iPhone's HEVC .mov on an Android phone or a Windows PC,
          // a codec the system lacks): say so plainly instead of a generic failure.
          UserFeedback::Fail(Tr(is_video ? "chat.attachment.video_unplayable" : "chat.attachment.open_failed"));
        }
      };
      log().info << "Attachment tap: lookup " << lookup_ms << " ms, worker " << worker_ms << " ms, to open call "
                 << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tapped).count()
                 << " ms" << (made_view ? " (made view)" : "");
      if (is_sound) {
        // A sound file reaches the system preview only when its card was not drawn as the in-app player (#111).
        log().info << "sound attachment tap: route=system-preview reason="
                   << (voice_playback_->PlaysFiles() ? "card drawn as a file (not ready when drawn)"
                                                     : "no in-app player on this platform");
      }
      if (needs_confirm) {
        dismiss_preview();
        ShowConfirm(Tr("chat.attachment.open_confirm_title"), Tr("chat.attachment.open_confirm_body"),
                    [open_file](const bool ok) {
                      if (ok) {
                        open_file();
                      }
                    });
        return;
      }
      open_file();
    });
  });
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
  voice_->Refresh(); // a call starting or ending greys / frees the microphone
  RefreshShowCapture();
  DirtyChatHeader();
}

void ChatController::ResetChatPanelState() {
  attachment_thread_.clear();
  chat_.attachment_uploading = false;
  chat_.attachment_failed = false;
  chat_.attachment_status = "";
  failed_record_id_.clear();
  pending_chip_thread_.clear();
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
    CloseSearch(); // no chat open (Home, or the chat was closed)
    chrome_.ResetPanelState();
    agent_.RefreshBusyForActiveThread();
    SyncDraftOwner();
    return;
  }
  const auto sync_started = std::chrono::steady_clock::now();
  const std::string thread_id = ActiveThreadId();
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id);
  if (thread_changed) {
    SyncComposerInputState(); // the upload chip and the locked input stay with the chat they belong to
  }
  // The attachment backfill reads the whole thread, so it does not run on every sync. New messages are
  // queued where they are received or sent; the backfill also re-queues failed downloads, which is why
  // it still runs now and then while the thread stays open (AttachmentBackfillPolicy.h).
  int64_t backfill_ms = 0;
  if (ShouldBackfillAttachments(thread_changed, sync_started, last_attachment_backfill_)) {
    last_attachment_backfill_ = sync_started;
    facade_->EnsureThreadAttachments(thread_id);
    backfill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  }

  const std::string prev_tail_id =
      chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
  const size_t prev_count = chat_.messages.size();

  chat_.messages = scroller_.Rows(thread_id, scroller_.LoadedMinDisplayOrder(), scroller_.LoadedMaxDisplayOrder());
  chat_.turns.clear();
  chat_.has_turns = !chat_.messages.empty();
  chat_.use_messages_layout = true;

  scroller_.EndDisplaySync(thread_changed, prev_tail_id, prev_count);
  if (search_.view().open) {
    if (thread_changed || thread_id != search_thread_) {
      CloseSearch();
    } else {
      // Messages added or removed (not a redraw): in the rows shown, or at the chat's end when the rows shown are
      // older history.
      const std::string tail =
          chat_.messages.empty() ? std::string() : std::string(chat_.messages.back().message_id.c_str());
      bool changed = tail != prev_tail_id || chat_.messages.size() != prev_count;
      if (changed) {
        if (!scroller_.LoadedMaxDisplayOrder()) {
          search_thread_tail_ = tail; // the rows shown end at the chat's end
        }
      } else if (auto newest = facade_->GetMessagesPage(thread_id, std::nullopt, 1); newest && !newest->empty()) {
        // The rows shown did not change; they may be older history: has the chat's end moved?
        changed = newest->back().id != search_thread_tail_;
        search_thread_tail_ = newest->back().id;
      }
      if (changed) {
        OnSearchMessagesChanged();
      }
    }
  }
  agent_.RefreshBusyForActiveThread(); // "thinking", the stop key and a queued bubble follow the open thread
  SyncDraftOwner();                    // so does the unsent text

  // Runs on the UI thread for every message, reaction and AI reply: report the slow ones.
  const auto sync_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  if (sync_ms >= kSlowDisplaySyncMs) {
    MetricsLine("chat.sync_slow")
        .Add("ms", static_cast<int64_t>(sync_ms))
        .Add("backfill_ms", backfill_ms) // the whole-thread attachment check, when it ran in this sync
        .Add("rows", static_cast<int64_t>(chat_.messages.size()))
        .Add("thread_changed", thread_changed)
        .Emit();
  }
}

// The delta's entry_id is the pending turn's user message id; the final AssistantReady carries the
// persisted assistant message id, so the live bubble is a synthetic last row that ClearStreamingRow
// drops right before FinishAssistantReply rebuilds the rows.




void ChatController::OpenChatLinkIn(const std::string& thread_id, const std::string& entry_id, const int link_index) {
  auto links = chat_links_.find(entry_id);
  if (links == chat_links_.end() && messaging_ready_ && facade_ && entry_id.rfind("streaming-", 0) != 0) {
    // After a restart the map is empty: rebuild from the message's own stored text.
    if (auto messages = facade_->GetMessagesPage(thread_id, std::nullopt, 10000)) {
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
  OpenTrustedUrl(*url); // chat_links_ holds the AI assistant's answers only (articles, sources)
}

bool ChatController::MessageFromAssistant(const std::string& thread_id, const std::string& entry_id) {
  if (!messaging_ready_ || !facade_) {
    return false;
  }
  auto found = facade_->GetMessage(thread_id, entry_id);
  return found && found->has_value() && (*found)->sender_contact_id == kAiAssistantContactId;
}

void ChatController::OpenTrustedUrl(const std::string& url) {
  if (IsOpenableUrl(url)) {
    (void)PlatformOpenUrl(url);
  }
}

void ChatController::ConfirmAndOpenUrl(const std::string& url) {
  if (!IsOpenableUrl(url)) {
    return;
  }
  if (skip_link_confirm_this_run_) {
    (void)PlatformOpenUrl(url);
    return;
  }
  // One fixed question and the host only, never the whole address (Kenneth 2026-11-04). The opt-out lives in
  // memory only: the next launch asks again.
  ShowConfirmWithCheckbox(Tr("chat.open_link_title"), Tr("chat.open_link_from", {{"host", UrlHost(url)}}),
                          Tr("chat.open_link_skip"), false,
                          [this, url](const bool ok, const bool skip) {
                            if (!ok) {
                              return;
                            }
                            skip_link_confirm_this_run_ = skip;
                            (void)PlatformOpenUrl(url);
                          },
                          Tr("chat.open_link_open"));
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

void ChatController::HandleLocalActionIn(const std::string& thread_id, const std::string& message,
                                         const std::optional<std::string>& payload, const std::string& entry_id) {
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
        switch (ReactionToResumeError(resumed.error().message)) {
        case ApprovalTapReaction::Silent:
          break;
        case ApprovalTapReaction::BusyElsewhere:
          ShowToast(Tr("chat.permission.busy_elsewhere"));
          break;
        case ApprovalTapReaction::StaleRewrite:
          ShowToast(Tr("chat.permission.stale"));
          agent_.CloseApprovalCard(thread_id, entry_id, ApprovalOutcome::Stale); // the card's chat, not the open one
          break;
        }
      }
      return;
    }
    if (action_type && *action_type == "local_action") {
      // A button of the on-device app-action flow. "Add a remark" opens the contact's page here; the rest runs
      // in the agent (MessageRouter hands it over, also in a person-to-person chat).
      if (action_json->getString("tool").value_or("") == "edit_remark") {
        const std::string contact_id = action_json->getString("contact_id").value_or("");
        if (!contact_id.empty() && contacts_notify_.select_contact) {
          contacts_notify_.select_contact(contact_id);
        }
        return;
      }
      if (!MessageRouter::IsAgentLocalActionPayload(payload)) {
        log().warning << "Local action with an unknown tool ignored";
        return;
      }
      SendUserText(message, payload);
      return;
    }
    if (action_type && *action_type == "open_url") {
      // An article card's button: straight to the browser when the AI assistant wrote it (our own feed), else the
      // host is confirmed first. https only either way.
      const std::string url = action_json->getString("url").value_or("");
      if (MessageFromAssistant(thread_id, entry_id)) {
        OpenTrustedUrl(url);
      } else {
        ConfirmAndOpenUrl(url);
      }
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
              ShowToast(LocalActionFailureText(result.error()));
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
    // Add / Message on a directory row look the person up first (HTTP, on a worker): the result may come later.
    if (local_action_in_flight_) {
      log().debug << "Local action ignored: a directory lookup is still pending";
      return;
    }
    local_action_in_flight_ = true;
    const std::string origin_thread_id = ActiveThreadId();
    facade_->DispatchActionAsync(*payload, [this, message, origin_thread_id](Roe<std::optional<std::string>> result) {
      local_action_in_flight_ = false;
      if (!result) {
        // Never re-route the same action payload through MessageRouter — that loops
        // HandleLocalAction → SendUserText → Route → HandleLocalAction until stack overflow.
        log().warning << "Local action failed: " << result.error().message;
        ShowToast(LocalActionFailureText(result.error()));
        NotifySurfaceChanged();
        return;
      }
      if (*result) {
        // The text belongs to the thread the row was tapped in; sending it elsewhere would put it in the wrong chat.
        if (ActiveThreadId() != origin_thread_id) {
          log().info << "Local action result dropped: the active thread changed while the lookup was pending";
          return;
        }
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
    });
    return;
  }
  SendUserText(message, payload);
}

void ChatController::OnSendMessage() {
  // Enter only checks focus: while an image is still being prepared the question must not go out
  // alone (the image would then attach itself to the next one).
  // A turn running in another thread does not block this composer (B112): SendUserText queues an AI send.
  if (chat_.compose_disabled || chat_.image_preparing) {
    return;
  }

  const std::string text = util::Trim(chat_.draft.c_str());
  const bool with_image = agent_.HasPendingImage() && InAiComposerContext();
  if (text.empty() && !with_image) {
    return;
  }

  // Sending keeps the draft focused, so the keyboard stays up for the next line (#90); a tap on the transcript
  // takes the focus and closes it (pp-cpp-ui moves the focus off the field on release).
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
    }
    return;
  }
  const std::string out = presenter_.ComposeWithQuote(text);
  if (!ComposerSendAllowed(out)) {
    return; // the draft stays: an AI send in the thread whose turn runs, or one already queued here
  }
  // Mentions mark the typed text only (a prefix of `out`), never the quote under it (G015).
  const std::string thread_id = ActiveThreadId();
  // Picks of members who left since are dropped here (read at send), as the receiver would not light them anyway.
  if (std::vector<TextSpan> spans = presenter_.SpansOnRoster(thread_id, mentions_.SpansFor(thread_id, text));
      presenter_.SendsWithMentions(thread_id, out, spans)) {
    SendWithMentions(thread_id, text, out, std::move(spans));
    return;
  }
  mentions_.ForgetThread(thread_id);
  chat_.draft = "";
  DirtyChatChrome();
  scroller_.RequestScrollToLatest();
  CancelQuoteReply();
  SendUserText(out);
}

void ChatController::SendWithMentions(const std::string& thread_id, const std::string& reply, const std::string& out,
                                      std::vector<TextSpan> spans) {
  mentions_.Close();
  chat_.draft = "";
  scroller_.RequestScrollToLatest();
  CancelQuoteReply();
  UpdateSidebarPreview(out);
  if (auto sent = presenter_.SendWithMentions(thread_id, out, std::move(spans)); !sent) {
    UserFeedback::Fail(PaymentErrorUserMessage(sent.error().message));
    RestoreDraftIfEmpty(thread_id, reply);  // its mentions stay recorded, so sending again marks them
  } else {
    mentions_.ForgetThread(thread_id);
  }
  DirtyChatChrome();
  SyncDisplayFromThread();
}

void ChatController::RegisterMentionPickerKeys() {
  if (!input_) {
    return;
  }
  for (const ui::Input::KeyIdentifier key : {ui::Input::KI_RETURN, ui::Input::KI_NUMPADENTER, ui::Input::KI_TAB}) {
    input_->Register(KeyBinding{
        .key = key,
        .forbidden_modifiers = ui::Input::KM_SHIFT,
        .when = [this](ui::Context* ctx) {
          ui::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
          return focus && focus->GetId() == "draft-input" && mentions_.IsOpen();
        },
        .action = [this]() { return !mentions_.ChooseFirst(); },
        .priority = 60, // before Enter sends
    });
  }
}

bool ChatController::ComposerSendAllowed(const std::string& text) {
  if (!messaging_ready_ || !facade_ || !facade_->HasRouter() || ChromeSnapshot().nav_tab == NavTab::Home) {
    return true; // a Home question opens its own thread; SendUserText decides there
  }
  const std::string thread_id = ActiveThreadId();
  const bool is_ai_send = !agent_.UseLlm() || facade_->ExpectsAgentWork(thread_id, text, std::nullopt);
  switch (DecideSend(is_ai_send, agent_.CanStart(thread_id), agent_.Busy().BusyIn(thread_id),
                     agent_.HasQueued(thread_id))) {
  case SendDecision::BlockedHere:
    log().info << "Send kept in the composer: a reply is still loading in this thread";
    return false;
  case SendDecision::AlreadyQueued:
    ShowToast(Tr("chat.ai_queue.already"));
    return false;
  case SendDecision::Send:
  case SendDecision::Queue:
    break;
  }
  return true;
}

void ChatController::OnNewChat() {
  if (!messaging_ready_) {
    return;
  }

  // The place the user leaves (Home or a chat) keeps its unsent text; the new chat starts empty, even when it is
  // opened from Home (nav may still say Home here, e.g. "find someone").
  const std::string leaving = chat_.draft.c_str();
  (void)facade_->CreateNewAiThread();
  const std::string new_thread = ActiveThreadId();
  if (!new_thread.empty()) {
    if (drafts_.Owner() && *drafts_.Owner() != new_thread) {
      StashComposerExtras(*drafts_.Owner()); // the quote / picked image stay where they were made
    }
    const std::string next = drafts_.SwitchTo(new_thread, leaving);
    chat_.draft = next.c_str();
    DataModelHost::Instance().Dirty("chat", "draft");
  }
  chat_.status = "";
  chat_.compose_disabled = false;
  // A turn running in another thread keeps running: its answer lands in its own thread (B112).
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

std::vector<std::string> ChatController::RecentEmojis() const {
  return session_store_ ? session_store_->Snapshot().profile_prefs.recent_emojis : std::vector<std::string>{};
}

void ChatController::RememberEmoji(const std::string& emoji) {
  if (!session_store_) {
    return;
  }
  ProfilePreferences prefs = session_store_->Snapshot().profile_prefs;
  const std::vector<std::string> before = prefs.recent_emojis;
  EmojiCatalog::TouchRecent(prefs.recent_emojis, emoji);
  if (prefs.recent_emojis == before) {
    return;  // already first: nothing to write
  }
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  (void)session_store_->SaveProfilePrefs(prefs);
}

void ChatController::DeleteMessageForMe(const std::string& message_id) {
  if (auto deleted = presenter_.DeleteForMe(message_id); !deleted) {
    ShowToast(AppError::Display(deleted.error()));
    return;
  }
  if (search_.DropHit(message_id)) {
    (void)search_.TakeJump(); // the count follows at once; the view stays where the user is
    ApplySearchView();
  }
  SyncDisplayFromThread(); // the rows lost one: the index is rebuilt (throttled)
  DirtyChatTurns();
  NotifySurfaceChanged();
}

void ChatController::DeleteMessageForEveryone(const std::string& message_id) {
  presenter_.DeleteForEveryone(message_id, [this](Roe<void> done) {
    if (!done) {
      ShowToast(AppError::Display(done.error()));
    }
    SyncDisplayFromThread(); // also after a failure: whatever did change is shown
    DirtyChatTurns();
    NotifySurfaceChanged();
  });
}

bool ChatController::IsContactCard(const std::string& message_id) const {
  if (!facade_ || message_id.empty()) {
    return false;
  }
  auto found = facade_->GetMessage(ActiveThreadId(), message_id);
  return found && found->has_value() && (*found)->content_type == ChatContentType::ContactCard;
}

void ChatController::AddContactFromCard(const std::string& message_id) {
  auto added = presenter_.AddContactFromCard(message_id);
  if (!added) {
    UserFeedback::Fail(added.error().message);
    return;
  }
  if (contacts_notify_.refresh) {
    contacts_notify_.refresh();
  }
  if (contacts_notify_.select_contact) {
    contacts_notify_.select_contact(*added);
  }
  NotifySurfaceChanged();
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
  // The original is looked for among the newest 500 messages (above). The scroller then loads its place when it is
  // outside the rows on screen, keeps it in view while images above it settle, and tints it for a moment; a
  // DOM-only lookup said "not found" for an original outside the rows on screen.
  if (target_id.empty() || scroller_.JumpToMessage(target_id) != ChatTranscriptScroller::JumpResult::Jumped) {
    ShowToast(Tr("chat.quote.not_found"));
  }
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
  // Phones: the bar under the header says which call goes on, and how many are in it (CallPresenter decides).
  const std::string bar = joinable && AskAiCompact() && call_actions_.call_join_bar_text
                              ? call_actions_.call_join_bar_text(thread_id)
                              : std::string();
  SetCallBarText(bar);
}

void ChatController::SetCallBarText(const std::string& text) {
  if (text != chat_.call_bar_text.c_str()) {
    chat_.call_bar_text = text.c_str();
    DataModelHost::Instance().Dirty("chat", "call_bar_text");
  }
}

void ChatController::OnJoinGroupCall() {
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty() || !call_actions_.start_call) {
    return;
  }
  // Read again: a call that ended since the bar was drawn is said so, and the bar goes.
  if (!call_actions_.call_joinable || !call_actions_.call_joinable(thread_id)) {
    SetCallBarText({});
    if (chat_.call_joinable) {
      chat_.call_joinable = false;
      DataModelHost::Instance().Dirty("chat", "call_joinable");
    }
    ShowToast(Tr("chat.call_bar.ended"));
    return;
  }
  (void)call_actions_.start_call(thread_id, false); // joins it; any other failure is the call's own error notice
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

void ChatController::PrefillDraft(const std::string& text) {
  if (text.empty() || chat_.compose_disabled) {
    return;
  }
  focus_draft_after_sync_ = true;
  if (util::Trim(chat_.draft.c_str()).empty()) {  // never overwrite what the user has typed
    chat_.draft = text.c_str();
  }
  DirtyChatChrome();
}

void ChatController::OnSessionSwipeAction(const std::string& thread_id, const SessionSwipeAction action) {
  menus_.RunSessionSwipeAction(thread_id, action);
}

void ChatController::OnShellLayoutSynced() {
  // SyncLayout remounts the shell DOM and resets scroll offsets — re-arm follow-tail.
  scroller_.OnShellRemounted();
  SyncHomeFavoriteSize();  // a turn or a resize changes the width the row divides
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
  // The phone keeps the new AI conversation on the AI 助手 page (Home): the chat overlay opens over it and Back
  // returns to the AI list. The desktop shows the chat in its panes, so it still moves to the Chats surface.
  if (ChromeSnapshot().layout_mode != LayoutMode::Compact) {
    ShellSelectNavTab(NavTab::Sessions);
  }
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

  bool home_thread_was_empty = false;
  // The composer on the PP 助手 page (the list, no chat overlay) starts a NEW conversation each send. Once a
  // conversation is open (the overlay), the same Home tab keeps sending into that thread — continue, don't re-create.
  if (ChromeSnapshot().nav_tab == NavTab::Home && !ChromeSnapshot().compact_chat_open) {
    if (!EnsureHomeOutboundSession(from_home_chip ? trimmed : std::string())) {
      log().info << "Send ignored: could not open a thread from Home";
      return;
    }
    auto first = facade_->GetMessagesPage(ActiveThreadId(), std::nullopt, 1);
    home_thread_was_empty = first && first->empty();
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

  // As many turns as the sessions allow: past that an AI send waits in the queue; a plain message always goes (B112).
  const std::string target_thread = ActiveThreadId();
  switch (DecideSend(expect_agent_work, agent_.CanStart(target_thread), agent_.Busy().BusyIn(target_thread),
                     agent_.HasQueued(target_thread))) {
  case SendDecision::Send:
    break;
  case SendDecision::BlockedHere:
    log().info << "Send ignored: a reply is still loading in this thread";
    return;
  case SendDecision::AlreadyQueued:
    ShowToast(Tr("chat.ai_queue.already"));
    return;
  case SendDecision::Queue:
    QueueAgentSend(target_thread, trimmed, std::move(user_payload), home_thread_was_empty);
    return;
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
    chat_.status = "";
  }
  // A fresh AI thread is named after its first question, so the list is not all "New chat". Done before
  // the preview update, which rebuilds the session rows once.
  (void)facade_->NameAiThreadFromFirstMessage(ActiveThreadId(), trimmed);
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
    if constexpr (kUiDemoBuild) {
      // The demo app: my message stays on the right and the answer comes from the other side, on the left. (The
      // mock reply below is keyed to my message's id, so it was written over my message, as if I had sent it.)
      std::optional<Thread> thread;
      if (auto found = facade_->GetThread(thread_id); found && *found) {
        thread = **found;
      }
      if (thread && thread->kind != ThreadKind::Ai && !thread->participant_contact_ids.empty()) {
        ThreadMessage reply;
        reply.id = util::GenerateUuid();
        reply.thread_id = thread_id;
        reply.sender_contact_id = thread->participant_contact_ids.front();
        reply.text = DemoPeerReply(trimmed);
        reply.timestamp = user_message.timestamp + 1;
        reply.transport = MessageTransport::Relay;
        reply.delivery = MessageDelivery::Relayed;
        (void)facade_->AppendMessage(reply);
        (void)facade_->UpdatePreview(thread_id, reply.text);
        SyncDisplayFromThread();
        SyncShellSessions();
        DirtyChatTurns();
        return;
      }
      // An AI chat: the mock answer as a new message from the assistant.
      agent_.MarkAgentBusy(thread_id);
      pending_reply_ = PendingReply{.entry_id = std::string(), .thread_id = thread_id,
                                    .output = MockAssistantRespond(trimmed), .from_llm = false};
      return;
    }
    log().debug << "Using mock assistant response";
    agent_.MarkAgentBusy(thread_id);
    pending_reply_ = PendingReply{.entry_id = user_message.id, .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed), .from_llm = false};
    return;
  }

  if (messaging_ready_ && facade_ && facade_->HasRouter()) {
    if (expect_agent_work && facade_->NeedsSharedAiConfirmFor(target_thread, trimmed)) {
      // Confirm "@ai+" sharing here (not inside the router) so a cancel keeps the text and "busy" stays right.
      ConfirmShareWithAi(target_thread, trimmed, [this, target_thread, trimmed](const bool ok) {
        if (!ok) {
          RestoreDraftIfEmpty(target_thread, trimmed);
          return;
        }
        if (agent_.CanStart(target_thread) != TurnStart::Yes) { // the AI filled up while the dialog was open
          QueueAgentSend(target_thread, trimmed, std::nullopt, false, /*already_confirmed=*/true);
          return;
        }
        auto routed = facade_->RouteConfirmedSharedAi(target_thread, trimmed);
        if (!routed) {
          if (routed.error().message == MessageRouter::kAgentBusyError) {
            QueueAgentSend(target_thread, trimmed, std::nullopt, false, /*already_confirmed=*/true);
          } else {
            UserFeedback::Fail(PaymentErrorUserMessage(routed.error().message));
          }
          return;
        }
        agent_.MarkAgentBusy(target_thread);
        SyncDisplayFromThread();
      });
      return;
    }
    log().info << "Routing message via MessageRouter";
    std::optional<std::string> payload_copy = user_payload;
    auto routed = facade_->RouteMessage(target_thread, trimmed, std::move(user_payload));
    if (!routed) {
      if (routed.error().message == MessageRouter::kAgentBusyError) {
        // The session took another turn the UI had not heard of yet: nothing was stored or sent, so queue it.
        log().warning << "Send refused by a busy assistant: queued";
        QueueAgentSend(target_thread, trimmed, std::move(payload_copy), home_thread_was_empty);
        return;
      }
      UserFeedback::Fail(PaymentErrorUserMessage(routed.error().message));
      DirtyChatChrome();
      SyncDisplayFromThread();
      return;
    }
    if (expect_agent_work) {
      agent_.MarkAgentBusy(target_thread); // same call stack as the submit: no window for another send
    }
    SyncDisplayFromThread();
    return;
  }

  log().info << "Submitting message to agent session";
  agent_.SubmitHomeQuestion(target_thread, trimmed, std::move(user_payload));
}

void ChatController::QueueAgentSend(const std::string& thread_id, const std::string& text,
                                    std::optional<std::string> payload, const bool thread_created_for_this,
                                    const bool already_confirmed) {
  if (!already_confirmed && facade_ && facade_->NeedsSharedAiConfirmFor(thread_id, text)) {
    // Asked now, in the chat the user is looking at, not when the queue reaches it.
    ConfirmShareWithAi(thread_id, text, [this, thread_id, text, payload, thread_created_for_this](const bool ok) {
      if (!ok) {
        RestoreDraftIfEmpty(thread_id, text);
        return;
      }
      QueueAgentSend(thread_id, text, payload, thread_created_for_this, /*already_confirmed=*/true);
    });
    return;
  }
  QueuedAgentSend item{.thread_id = thread_id,
                       .text = text,
                       .payload = std::move(payload),
                       .shared_confirmed = already_confirmed,
                       .thread_created_for_this = thread_created_for_this};
  if (!agent_.QueueSend(std::move(item))) {
    ShowToast(Tr("chat.ai_queue.already"));
    RestoreDraftIfEmpty(thread_id, text);
    return;
  }
  scroller_.RequestScrollToLatest();
  SyncShellSessions();
}

void ChatController::ConfirmShareWithAi(const std::string& thread_id, const std::string& text,
                                        std::function<void(bool ok)> then) {
  ShowShareWithAiDialog(thread_id, ParseAtAiPrefix(text).mode,
                        [then = std::move(then)](const bool ok, bool /*dont_ask*/) { then(ok); });
}

void ChatController::ShowShareWithAiDialog(const std::string& thread_id, const AtAiMode mode,
                                           std::function<void(bool ok, bool dont_ask)> done) {
  ShowConfirmWithCheckbox(Tr("chat.share_ai.title"),
                          Tr(mode == AtAiMode::SharedFull ? "chat.share_ai.body_full" : "chat.share_ai.body_reply"),
                          Tr("chat.share_ai.dont_ask"), false,
                          [this, thread_id, done = std::move(done)](const bool ok, const bool dont_ask) {
                            if (ok && dont_ask && facade_) {
                              facade_->MarkSharedAiConfirmed(thread_id);
                              presenter_.ForgetCachedPrefs();
                            }
                            done(ok, dont_ask);
                          });
}

void ChatController::StashComposerExtras(const std::string& key) {
  if (presenter_.QuotingReply()) {
    quotes_[key] = presenter_.QuoteSource();
  } else {
    quotes_.erase(key);
  }
  CancelQuoteReply();
  agent_.StashPendingImage(key);
}

void ChatController::RestoreComposerExtras(const std::string& key) {
  if (auto it = quotes_.find(key); it != quotes_.end()) {
    presenter_.StartQuoteReply(it->second);
    chat_.quote_reply = true;
    chat_.quote_reply_text = StructuredTextParser::EscapeText(presenter_.QuotePreview()).c_str();
    DataModelHost::Instance().Dirty("chat", "quote_reply");
    DataModelHost::Instance().Dirty("chat", "quote_reply_text");
    quotes_.erase(it);
  }
  agent_.RestorePendingImage(key);
}

void ChatController::ForgetComposerExtras(const std::string& key) {
  drafts_.Forget(key);
  quotes_.erase(key);
  mentions_.ForgetThread(key);
  agent_.ForgetPendingImage(key);
  if (agent_.Ports().forget_thread) {
    agent_.Ports().forget_thread(key); // the AI session's memory of which role answered last
  }
}

void ChatController::RestoreDraftIfEmpty(const std::string& thread_id, const std::string& text) {
  PutTextBack(thread_id, text, /*prepend=*/false);
}

void ChatController::PutBackDraft(const std::string& thread_id, const std::string& text) {
  PutTextBack(thread_id, text, /*prepend=*/true);
}

std::string ChatController::CurrentDraftKey() const {
  return ChromeSnapshot().nav_tab == NavTab::Home ? std::string(ThreadDrafts::kHome) : ActiveThreadId();
}

void ChatController::PutTextBack(const std::string& key, const std::string& text, const bool prepend) {
  SyncDraftOwner(); // so "shown" below is what the user sees now
  if (key.empty() || key != CurrentDraftKey()) {
    // Not the conversation on screen: its own composer gets the text, never the one shown.
    if (prepend) {
      drafts_.Prepend(key, text);
    } else if (drafts_.Peek(key).empty()) {
      drafts_.Put(key, text);
    }
    return;
  }
  const std::string draft = chat_.draft.c_str();
  if (!prepend && !draft.empty()) {
    return;
  }
  chat_.draft = (draft.empty() ? text : text + "\n" + draft).c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
}

void ChatController::SyncDraftOwner() {
  const std::string key = CurrentDraftKey();
  if (key.empty()) {
    return; // no conversation shown: the text stays with the one it was typed in
  }
  if (drafts_.Owner() && *drafts_.Owner() == key) {
    return;
  }
  // The quote and the picked image stay with the conversation they were made in, like the text.
  if (drafts_.Owner()) {
    StashComposerExtras(*drafts_.Owner());
  }
  RestoreComposerExtras(key);
  const std::string current = chat_.draft.c_str();
  const std::string next = drafts_.SwitchTo(key, current);
  if (next != current) {
    chat_.draft = next.c_str();
    DataModelHost::Instance().Dirty("chat", "draft");
  }
}

QueuedSendOutcome ChatController::RunQueuedAgentSend(const QueuedAgentSend& item) {
  if (!messaging_ready_ || !facade_ || !facade_->HasRouter()) {
    return QueuedSendOutcome::NotStarted;
  }
  if (!agent_.UseLlm()) {
    log().info << "Queued AI send dropped: no assistant configured";
    return QueuedSendOutcome::NotStarted;
  }
  const bool expects = item.shared_confirmed || facade_->ExpectsAgentWork(item.thread_id, item.text, item.payload);
  auto routed = item.shared_confirmed ? facade_->RouteConfirmedSharedAi(item.thread_id, item.text)
                                      : facade_->RouteMessage(item.thread_id, item.text, item.payload);
  if (!routed) {
    if (routed.error().message == MessageRouter::kAgentBusyError) {
      return QueuedSendOutcome::Busy;
    }
    log().warning << "Queued AI send failed: " << routed.error().message;
    if (item.thread_id == ActiveThreadId()) {
      UserFeedback::Fail(PaymentErrorUserMessage(routed.error().message));
    }
    return QueuedSendOutcome::NotStarted;
  }
  (void)facade_->NameAiThreadFromFirstMessage(item.thread_id, item.text);
  if (!expects) {
    return QueuedSendOutcome::NotStarted;
  }
  agent_.MarkAgentBusy(item.thread_id);
  SyncShellSessions(); // the thread gets its preview / unread mark even if it is not the open one
  if (item.thread_id == ActiveThreadId()) {
    SyncDisplayFromThread();
  }
  return QueuedSendOutcome::Started;
}

void ChatController::CancelQueuedHere() {
  const std::string thread_id = ActiveThreadId();
  std::optional<QueuedAgentSend> item = agent_.TakeQueued(thread_id);
  if (!item) {
    return;
  }
  log().info << "Queued AI send cancelled";
  if (item->thread_created_for_this && facade_) {
    // A Home question opened this thread for nothing: do not leave an empty chat in the list.
    auto first = facade_->GetMessagesPage(thread_id, std::nullopt, 1);
    if (first && first->empty()) {
      CloseChatWith(thread_id, [this, thread_id]() { return presenter_.CloseThread(thread_id); });
      // Closing opens another thread, maybe a chat with a person: the AI question goes back to Home's composer,
      // never into theirs (one Enter would send it to them).
      auto open = facade_->GetActiveThread();
      if (!open || open->kind != ThreadKind::Ai) {
        ShellSelectNavTab(NavTab::Home);
      }
      // The question was asked on Home: it goes back to Home's composer, not to whichever chat opened now.
      PutTextBack(ThreadDrafts::kHome, item->text, /*prepend=*/false);
      return;
    }
  }
  PutTextBack(thread_id, item->text, /*prepend=*/false);
}

ChatController::ComposerEscape ChatController::OnComposerEscape() {
  if (voice_ && voice_->OnEscape()) {
    return ComposerEscape::Handled; // Esc cancels a recording (the composer is not focused during one)
  }
  if (chat_.search_open) {
    CloseSearch(/*refocus_composer=*/true); // the search box has the focus, not the composer
    return ComposerEscape::Handled;
  }
  if (!context_) {
    return ComposerEscape::NotInComposer;
  }
  ui::Element* focus = context_->GetFocusElement();
  if (!focus || focus->GetId() != "draft-input") {
    return ComposerEscape::NotInComposer;
  }
  if (mentions_.IsOpen()) {
    mentions_.Close();  // the `@` picker first; the draft keeps its `@`
    return ComposerEscape::Handled;
  }
  const int64_t now = util::NowUnixMs();
  const std::string thread_id = ActiveThreadId();
  const EscapeAction action =
      DecideEscape(now, last_escape_ms_, agent_.HasQueued(thread_id), agent_.Busy().BusyIn(thread_id));
  last_escape_ms_ = now;
  switch (action) {
  case EscapeAction::CancelQueued:
    CancelQueuedHere();
    return ComposerEscape::Handled;
  case EscapeAction::StopTurn:
    last_escape_ms_.reset();
    agent_.OnStopTurn();
    return ComposerEscape::Handled;
  case EscapeAction::Armed:
    return ComposerEscape::Handled; // a second press stops; leaving the chat now would make that impossible
  case EscapeAction::None:
    break;
  }
  return ComposerEscape::Idle;
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
      [this](std::string title, std::string body, std::string thread_id, bool mentions_us) {
        const ProfilePreferences& profile_prefs = Store().Snapshot().profile_prefs;
        if (!profile_prefs.show_notifications) {
          return;
        }
        body = NotificationBodyFor(profile_prefs.show_notification_preview, std::move(body),
                                   Tr("notification.hidden_body"));
        // A muted thread still counts as unread in the list; it just does not notify — unless the message
        // mentions us (G015), and then once a minute per group at most.
        const std::optional<ThreadLocalPrefs> prefs = presenter_.PrefsOf(thread_id);
        const int64_t now_ms = util::NowUnixMs();
        const bool muted = prefs && ThreadIsMuted(prefs->muted_until, now_ms);
        if (!NoticeAlerts(muted, prefs && prefs->mute_mentions, mentions_us, muted_mention_alert_ms_[thread_id],
                          now_ms)) {
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
  const auto activate = [this](std::string thread_id) {
    DesktopWindowChrome::RaiseAndFocus();
    if (!thread_id.empty()) {
      OnSelectThread(thread_id);
    }
  };
  ILocalNotifier::Instance().SetActivationHandler(activate);
  // A message alert's tap waits for the message it announced to sync, then opens its chat.
  facade_->SetOnInboundMessage([this](std::string message_id, std::string thread_id) {
    if (PendingNotificationTap::Instance().TakeMessageRoute(message_id, PendingNotificationTap::Clock::now())) {
      OnSelectThread(thread_id);
    }
  });
  // A tap that arrived before unlock found no handler; it is still waiting.
  if (auto pending = PendingNotificationTap::Instance().TakeThread()) {
    activate(std::move(*pending));
  }
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
          HandleLocalAction(message, payload, {});
        });
    facade_->SetSharedAiConfirmCallback(
        [this](const std::string& thread_id, const AtAiMode mode, const std::string& /*prompt*/,
               std::function<void(bool confirmed, bool dont_ask_again)> done) {
          ShowShareWithAiDialog(thread_id, mode, std::move(done));
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

void ChatController::BindSharedEvents(ui::DataModelConstructor& ctor) {
  ctor.BindEventCallback("new_message", DataEvent([this] { OnNewMessage(); }));
  ctor.BindEventCallback("send_chat_action", DataEvent<std::string, int>(
      [this](const std::string& entry_id, int index) {
                           if (index >= 0) {
                             agent_.SendChatAction(entry_id, index);
                           }
                         }));
  ctor.BindEventCallback("submit_form", DataEvent<std::string, std::string>(
                                            [this](const std::string& form_id, const std::string& values_json) {
                                              agent_.SubmitForm(form_id, values_json);
                                            }));
  ctor.BindEventCallback("calendar_prev",
                         DataEvent<std::string>([this](const std::string& entry_id) { agent_.CalendarPrev(entry_id); }));
  ctor.BindEventCallback("calendar_next",
                         DataEvent<std::string>([this](const std::string& entry_id) { agent_.CalendarNext(entry_id); }));
  ctor.BindEventCallback("select_calendar_day", DataEvent<std::string, std::string>(
                                                    [this](const std::string& entry_id, const std::string& day) {
                                                      agent_.SelectCalendarDay(entry_id, day);
                                                    }));
  ctor.BindEventCallback("open_working_set", DataEvent<std::string, int>(
      [this](const std::string& entry_id, int block) {
                           if (block >= 0) {
                             working_set_.Open(entry_id, block);
                           }
                         }));
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
  RegisterMentionPickerKeys();
  // Enter in the chat's search box: the next older hit (the query just typed is searched first).
  for (const ui::Input::KeyIdentifier key : {ui::Input::KI_RETURN, ui::Input::KI_NUMPADENTER}) {
    if (!input_) {
      break;
    }
    input_->Register(KeyBinding{
        .key = key,
        .when =
            [](ui::Context* ctx) {
              ui::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
              return focus && focus->GetId() == "search-input";
            },
        .action =
            [this]() {
              OnSearchEnter();
              return false;
            },
        .priority = 50,
    });
  }

  if (!DataModelHost::Instance().Register(context, "chat", [this](ui::DataModelConstructor& ctor) {
        BindChatScreenState(ctor, chat_);
        ctor.BindEventCallback("send_message", DataEvent([this] { OnSendMessage(); }));
        ctor.BindEventCallback("send_suggestion", DataEvent<std::string>([this](const std::string& text) {
          // Phones: Get started / What can you do raise the ask-AI panel over Home and ask there (PLAN-ui-batch5-ask-ai).
          if (ChromeSnapshot().nav_tab == NavTab::Home && ask_ai_.OpenAndAsk(AskAiEntry::HomeChip, text)) {
            return;
          }
          // A Home chip continues its own thread instead of opening a new one on every tap.
          SendUserText(text, std::nullopt, /*from_home_chip=*/true);
        }));
        // Today's headlines go to the AI chat page as before, phones too (Kenneth 2026-11-04: users follow up on the
        // news there).
        ctor.BindEventCallback("send_suggestion_in_chat", DataEvent<std::string>([this](const std::string& text) {
          SendUserText(text, std::nullopt, /*from_home_chip=*/true);
        }));
        ctor.BindEventCallback("send_example", DataEvent<std::string>([this](const std::string& text) {
          // Inside an open thread: send there, never re-target to a chip's own thread.
          SendUserText(text, std::nullopt, /*from_home_chip=*/false);
        }));
        ctor.BindEventCallback("send_suggestion_action", DataEvent<std::string>(
            [this](const std::string& id) { SendSuggestionAction(id); }));
        ctor.BindEventCallback("open_chat_link", DataEvent<std::string, int>(
            [this](const std::string& entry_id, int link) { OpenChatLink(entry_id, link); }));
        ctor.BindEventCallback("stop_turn", DataEvent([this] { agent_.OnStopTurn(); }));
        ctor.BindEventCallback("cancel_queued", DataEvent([this] { CancelQueuedHere(); }));
        ctor.BindEventCallback("toggle_reaction", DataEvent<std::string, std::string>(
            [this](const std::string& message_id, const std::string& emoji) { ToggleReaction(message_id, emoji); }));
        ctor.BindEventCallback("open_emoji_insert", DataEventAt(
            [this](ui::Event& ev) { menus_.OpenEmojiInsert(&ev); }));
        ctor.BindEventCallback("attach_file", DataEventAt([this](ui::Event& ev) { OnAttachFile(&ev); }));
        ctor.BindEventCallback("home_attach_menu", DataEventAt([this](ui::Event& ev) {
          if (!messaging_ready_ || !facade_ || chat_.composer_input_disabled || chat_.attachment_uploading ||
              chat_.attachment_preparing || !chat_.show_attach_button) {
            return;
          }
          agent_.ShowHomeAttachMenu(ev.GetCurrentElement());
        }));
        ctor.BindEventCallback("attach_capture", DataEventAt([this](ui::Event& ev) { OnAttachCapture(&ev); }));
        ctor.BindEventCallback("voice_mode_toggle", DataEvent([this] { ToggleVoiceMode(); }));
        ctor.BindEventCallback("voice_press", DataEventAt([this](ui::Event& ev) { voice_->OnPress(ev); }));
        ctor.BindEventCallback("voice_click", DataEventAt([this](ui::Event& ev) { voice_->OnClick(ev); }));
        ctor.BindEventCallback("voice_send", DataEvent([this] { voice_->OnSendButton(); }));
        ctor.BindEventCallback("voice_cancel", DataEvent([this] { voice_->OnCancelButton(); }));
        ctor.BindEventCallback("play_voice", DataEvent<std::string>(
            [this](const std::string& message_id) { voice_playback_->Toggle(message_id); }));
        ctor.BindEventCallback("play_audio_file", DataEvent<std::string>(
            [this](const std::string& message_id) { voice_playback_->ToggleFile(message_id); }));
        ctor.BindEventCallback("audio_seek_press", DataEventAt([this](ui::Event& ev) { voice_playback_->SeekPress(ev); }));
        ctor.BindEventCallback("retry_attachment_send", DataEvent([this] { OnRetryAttachmentSend(); }));
        ctor.BindEventCallback("discard_attachment_send", DataEvent([this] { OnDiscardAttachmentSend(); }));
        ctor.BindEventCallback("cancel_attachment_send", DataEvent([this] { OnCancelAttachmentSend(); }));
        ctor.BindEventCallback("remove_image", DataEvent([this] { agent_.DiscardPendingAiImage(); }));
        ctor.BindEventCallback("cancel_quote_reply", DataEvent([this] { CancelQuoteReply(); }));
        ctor.BindEventCallback("jump_to_quote", DataEvent<std::string>(
            [this](const std::string& reply_id) { JumpToQuotedMessage(reply_id); }));
        ctor.BindEventCallback("open_message_link", DataEvent<std::string, int>(
            [this](const std::string& message_id, int link) { OpenMessageLink(message_id, link); }));
        ctor.BindEventCallback("draft_changed", DataEvent([this] { mentions_.OnDraftChanged(); }));
        ctor.BindEventCallback("open_mention", DataEvent<std::string, int>([this](const std::string& message_id, int run) {
          // A highlighted mention: the member's contact page; nothing for someone who is not a contact.
          const auto contact =
              run < 0 ? std::nullopt : presenter_.MentionedContact(message_id, static_cast<size_t>(run));
          if (contact && contacts_notify_.select_contact) {
            contacts_notify_.select_contact(*contact);
          }
        }));
        ctor.BindEventCallback("open_sender", DataEvent<std::string>(
            [this](const std::string& message_id) { OpenGroupSender(message_id); }));
        ctor.BindEventCallback("open_attachment", DataEvent<std::string>(
            [this](const std::string& message_id) { OpenAttachment(message_id); }));
        ctor.BindEventCallback("open_picture", DataEvent<std::string, std::string>(
            [this](const std::string& message_id, const std::string& local_path) {
              OpenPicture(message_id, local_path);
            }));
        ctor.BindEventCallback("download_attachment", DataEvent<std::string>(
            [this](const std::string& message_id) { DownloadAttachment(message_id); }));
        ctor.BindEventCallback("retry_attachment", DataEvent<std::string>(
            [this](const std::string& message_id) { RetryAttachmentDownload(message_id); }));
        ctor.BindEventCallback("clear_history", DataEvent([this] { OnClearHistory(); }));
        ctor.BindEventCallback("forget_memory", DataEvent([this] { OnForgetMemory(); }));
        ctor.BindEventCallback("open_search", DataEvent([this] { OnOpenSearch(); }));
        ctor.BindEventCallback("close_search", DataEvent([this] { CloseSearch(/*refocus_composer=*/true); }));
        ctor.BindEventCallback("search_query_changed", DataEvent([this] { OnSearchQueryChanged(); }));
        ctor.BindEventCallback("search_older", DataEvent([this] { OnSearchOlder(); }));
        ctor.BindEventCallback("search_newer", DataEvent([this] { OnSearchNewer(); }));
        ctor.BindEventCallback("open_thread_actions_menu", DataEventAt(
            [this](ui::Event& ev) { menus_.OpenThreadActions(ev); }));
        ctor.BindEventCallback("start_call", DataEventAt([this](ui::Event& ev) { menus_.StartCall(ev); }));
        ctor.BindEventCallback("join_group_call", DataEvent([this] { OnJoinGroupCall(); }));
        ctor.BindEventCallback("open_group_members", DataEvent([this] { peer_sheet_.OpenGroupMembers(); }));
        ctor.BindEventCallback("open_peer_sheet", DataEventAt([this](ui::Event& ev) { OpenPeerSheet(ev); }));
        ctor.BindEventCallback("sync_with_peer", DataEvent([this] { OnSyncWithPeer(); }));
        ctor.BindEventCallback("retry_gap_sync", DataEvent([this] { OnRetryGapSync(); }));
        ctor.BindEventCallback("start_new_secure_chat", DataEvent([this] { OnStartNewSecureChat(); }));
        ctor.BindEventCallback("pause_integrity_only", DataEvent([this] { OnPauseIntegrityOnly(); }));
        ctor.BindEventCallback("copy_psk_key", DataEvent([this] { OnCopyPskKey(); }));
        ctor.BindEventCallback("toggle_psk_import", DataEvent([this] { OnTogglePskImport(); }));
        ctor.BindEventCallback("import_psk", DataEvent([this] { OnImportPsk(); }));
        ctor.BindEventCallback("verify_psk", DataEvent([this] { OnVerifyPsk(); }));
        ctor.BindEventCallback("rotate_psk_export", DataEvent([this] { OnRotatePskExport(); }));
        ctor.BindEventCallback("load_older_history", DataEvent([this] { OnLoadOlderHistory(); }));
        ctor.BindEventCallback("retry_peer_dial", DataEvent([this] { OnRetryPeerDial(); }));
        ctor.BindEventCallback("messages_scroll", DataEvent([this] { OnMessagesScroll(); }));
        ctor.BindEventCallback("jump_to_latest", DataEvent([this] { OnJumpToLatest(); }));
        BindSharedEvents(ctor);
      })) {
    return false;
  }
  if (!ask_ai_.Register(context)) {
    return false;
  }
  if (!group_info_.Register(context)) {
    return false;
  }
  if (!group_leave_.Register(context)) {
    return false;
  }
  if (!group_link_->Register(context)) {
    return false;
  }
  if constexpr (kUiDemoBuild) {
    ask_ai_.SetFakeAnswers(DemoAskAiAnswer); // the demo app has no AI backend
  }

  if (!DataModelHost::Instance().Register(context, "shell", [this](ui::DataModelConstructor& ctor) {
        BindChatShellState(ctor, shell_);
        ctor.BindEventCallback("sessions_search_changed", DataEvent([this] { OnSessionsSearchChanged(); }));
        ctor.BindEventCallback("toggle_archived", DataEvent([this] { OnToggleArchived(); }));
        ctor.BindEventCallback("toggle_archived_ai", DataEvent([this] { OnToggleArchivedAi(); }));
        ctor.BindEventCallback("sessions_scroll", DataEvent([this] { OnSessionsScroll(); }));
        ctor.BindEventCallback("sessions_load_more", DataEvent([this] { OnSessionsLoadMore(); }));
        ctor.BindEventCallback("new_chat", DataEvent([this] { OnNewChat(); }));
        ctor.BindEventCallback("open_new_session_menu", DataEventAt([this](ui::Event& ev) {
          // The phone goes straight to a full-screen page; the desktop keeps its small menu.
          if (ChromeSnapshot().layout_mode == LayoutMode::Compact && contacts_notify_.open_new_chat) {
            contacts_notify_.open_new_chat();
            return;
          }
          menus_.OpenNewSession(ev);
        }));
        ctor.BindEventCallback("select_thread", DataEvent<std::string>(
            [this](const std::string& thread_id) { OnSelectThread(thread_id); }));
        ctor.BindEventCallback("home_open_brief", DataEvent<int>([this](const int index) { OpenHomeBrief(index); }));
        ctor.BindEventCallback("home_brief_all", DataEvent([this] { SendSuggestionAction("articles"); }));
        ctor.BindEventCallback("home_open_new_people", DataEvent([this] { OpenHomeNewPeople(); }));
        ctor.BindEventCallback("home_open_security", DataEvent([this] {
          if (open_settings_section_) {
            open_settings_section_("security");
          }
        }));
        ctor.BindEventCallback("home_open_favorite", DataEvent<int>([this](const int index) { OpenHomeFavorite(index); }));
        ctor.BindEventCallback("home_open_calls", DataEvent([this] { OpenHomeCalls(); }));
        ctor.BindEventCallback("home_open_chats", DataEvent([this] { ShellSelectNavTab(NavTab::Sessions); }));
        ctor.BindEventCallback("home_open_mentions", DataEvent([this] { OpenHomeMentions(); }));
        ctor.BindEventCallback("home_edit_open", DataEvent([this] { OpenHomeEdit(); }));
        ctor.BindEventCallback("home_widget_remove", DataEvent<std::string>(
            [this](const std::string& id) { SaveHomeLayout(RemoveHomeWidget(HomeLayout(), id)); }));
        ctor.BindEventCallback("home_widget_add", DataEvent<std::string>(
            [this](const std::string& id) { SaveHomeLayout(AddHomeWidget(HomeLayout(), id)); }));
        ctor.BindEventCallback("home_edit_drag_start", DataEvent<std::string>(
            [this](const std::string& id) { OnHomeEditDragStart(id); }));
        ctor.BindEventCallback("home_edit_drag", DataEventAt([this](ui::Event& ev) { OnHomeEditDrag(ev); }));
        ctor.BindEventCallback("home_edit_drag_end", DataEvent([this] { OnHomeEditDragEnd(); }));
        BindSharedEvents(ctor);
      })) {
    return false;
  }

  if (shell_setup_.initialize) {
    shell_setup_.initialize(context);
  }

  ContextMenuHost::Instance().RegisterProvider(
      [this](const ContextMenuRequest& request) { return menus_.ForRequest(request); });
  ContextMenuHost::Instance().SetTitleResolver(
      [this](const ContextMenuRequest& request) { return menus_.MenuTitleFor(request); });
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
      {.key = "new_chat", .rml_path = "views/new_chat.rml", .role = PaneRole::Primary});
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
  UpdateThreadChrome();
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
  UpdateThreadChrome();
  DirtyChatChrome();
  NotifySurfaceChanged();
}




void ChatController::CancelVoice() {
  if (voice_) {
    voice_->DiscardAll();
  }
  if (voice_playback_) {
    voice_playback_->Stop();
  }
}

void ChatController::OnApplicationPause() {
  CloseSearch(); // the decrypted index does not stay in memory while the app is in the background
  agent_.Cancel();
  // Going to the background loses a held press (no release will come) and ends a clip: cancel, never send.
  voice_->Lost();
  voice_playback_->Stop();
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
  ask_ai_.Tick(); // the panel's streamed answer, a one-off waiting to start, focus and scroll
  group_info_.Tick(messaging_ready_ ? ActiveThreadId() : std::string(), std::chrono::steady_clock::now());
  group_leave_.Tick(std::chrono::steady_clock::now());
  group_link_->Tick();
  join_prompt_->Tick(std::chrono::steady_clock::now());

  if (search_.view().open) {
    const auto search_now = std::chrono::steady_clock::now();
    if (search_query_due_ && search_now >= *search_query_due_) {
      search_query_due_.reset();
      search_.SetQuery(chat_.search_query.c_str());
      ApplySearchView();
      JumpToSearchHit();
    }
    if (const std::optional<uint64_t> generation = search_.Tick(SteadyNowMs())) {
      StartSearchIndex(*generation);
    }
  }

  // Voice: the hold's touch state, a call turning live, and the vault locking under a recording or a clip.
  const auto now = std::chrono::steady_clock::now();
  voice_->Tick(now);
  if (preparing_.Expire(now)) {
    log().warning << "attachment preparing timed out";
    SyncAttachmentPreparing();
  }
  if ((voice_->Active() || voice_playback_->Active()) && messaging_ready_ && !MessagingReady()) {
    CancelVoice();
  }
  if (voice_playback_->Active() && now - last_voice_call_check_ >= std::chrono::milliseconds(250)) {
    last_voice_call_check_ = now;
    if (facade_ && facade_->AnyCallLive()) {
      voice_playback_->OnCallLive();
    }
  }
}

void ChatController::AfterLayout() {
  if (search_focus_ != SearchFocus::None && context_ && context_->GetNumDocuments() > 0) {
    const char* id = search_focus_ == SearchFocus::Input ? "search-input" : "draft-input";
    search_focus_ = SearchFocus::None;
    if (ui::Element* el = context_->GetDocument(0)->GetElementById(id)) {
      el->Focus();
    }
  }
  scroller_.ApplyPolicy();
  FillSessionListAfterLayout();
}

void ChatController::Shutdown() {
  voice_->Shutdown();
  voice_playback_->Shutdown();
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
    // Unhook first: a configure that starts after this must not reach the messaging hub that is going away.
    if (agent_.Ports().set_tool_registration_hook) {
      agent_.Ports().set_tool_registration_hook({});
    }
    if (agent_.Ports().cancel_all) {
      agent_.Ports().cancel_all();
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

// --- Search inside the open chat -------------------------------------------------------------------------------

void ChatController::OnOpenSearch() {
  const std::string thread_id = ActiveThreadId();
  if (!messaging_ready_ || !facade_ || thread_id.empty() || (voice_ && voice_->Recording())) {
    return;
  }
  if (search_.view().open && search_thread_ == thread_id) {
    search_focus_ = SearchFocus::Input;
    return;
  }
  // The search box and the step bar take the header's and the composer's places: what floats over them goes.
  mentions_.Close();
  if (emoji_picker_notify_.close_if_open) {
    emoji_picker_notify_.close_if_open();
  }
  search_thread_ = thread_id;
  search_thread_tail_.clear();
  if (auto newest = facade_->GetMessagesPage(thread_id, std::nullopt, 1); newest && !newest->empty()) {
    search_thread_tail_ = newest->back().id;
  }
  const uint64_t generation = search_.Open();
  search_query_due_.reset();
  chat_.search_query = "";
  DataModelHost::Instance().Dirty("chat", "search_query");
  ApplySearchView();
  search_focus_ = SearchFocus::Input;
  StartSearchIndex(generation);
}

void ChatController::CloseSearch(const bool refocus_composer) {
  if (!search_.view().open && !chat_.search_open) {
    return;
  }
  search_.Close();
  search_thread_.clear();
  search_thread_tail_.clear();
  search_query_due_.reset();
  chat_.search_query = "";
  DataModelHost::Instance().Dirty("chat", "search_query");
  ApplySearchView();
  search_focus_ = refocus_composer ? SearchFocus::Composer : SearchFocus::None;
}

void ChatController::OnSearchQueryChanged() {
  if (!search_.view().open) {
    return;
  }
  search_query_due_ = std::chrono::steady_clock::now() + kSearchDebounce;
}

void ChatController::OnSearchEnter() {
  if (!search_.view().open) {
    return;
  }
  if (search_query_due_ || chat_.search_query.c_str() != search_.view().query) {
    search_query_due_.reset(); // typed and not yet searched: search it now, at its newest hit
    search_.SetQuery(chat_.search_query.c_str());
    ApplySearchView();
    JumpToSearchHit();
    return;
  }
  OnSearchOlder();
}

void ChatController::OnSearchOlder() {
  search_.Older();
  ApplySearchView();
  JumpToSearchHit();
}

void ChatController::OnSearchNewer() {
  search_.Newer();
  ApplySearchView();
  JumpToSearchHit();
}

void ChatController::ApplySearchView() {
  const ThreadSearchSession::View& view = search_.view();
  chat_.search_open = view.open;
  chat_.search_indexing = view.open && view.indexing;
  chat_.search_has_hits = view.hit_count > 0;
  chat_.search_can_older = view.can_older;
  chat_.search_can_newer = view.can_newer;
  std::string label;
  if (view.open) {
    if (view.indexing) {
      label = Tr("chat.search.indexing");
    } else if (!FoldForSearch(view.query).empty()) {
      label = view.hit_count == 0 ? Tr("chat.search.none")
                                  : Tr("chat.search.count", {{"index", std::to_string(view.current + 1)},
                                                             {"count", std::to_string(view.hit_count)}});
    }
  }
  chat_.search_count_label = label.c_str();

  search_hit_ids_.clear();
  for (const ThreadSearchHit& hit : search_.hits()) {
    search_hit_ids_.insert(hit.message_id);
  }
  search_current_id_ = search_.CurrentMessageId();
  search_folded_query_ = view.open ? FoldForSearch(view.query) : std::string();
  if (view.open) {
    scroller_.SetRowDecorator([this](std::vector<MessageDisplayRow>& rows) {
      MarkSearchRows(rows, search_hit_ids_, search_current_id_, search_folded_query_);
    });
  } else {
    scroller_.SetRowDecorator({});
  }
  scroller_.Redecorate(); // the rows shown now: a new query or a step changes the tint, not the rows

  for (const char* field : {"search_open", "search_indexing", "search_count_label", "search_has_hits",
                            "search_can_older", "search_can_newer"}) {
    DataModelHost::Instance().Dirty("chat", field);
  }
}

void ChatController::JumpToSearchHit() {
  // Each NotFound drops one hit, so this ends.
  while (std::optional<std::string> message_id = search_.TakeJump()) {
    if (scroller_.JumpToMessage(*message_id) != ChatTranscriptScroller::JumpResult::NotFound) {
      return; // jumped; or the store did not answer (not proof that it is gone: the hit stays)
    }
    search_.DropHit(*message_id); // deleted since the index was built
    ApplySearchView();
  }
}

void ChatController::StartSearchIndex(const uint64_t generation) {
  ConversationsFacade* facade = facade_;
  const std::string thread_id = search_thread_;
  if (!facade || thread_id.empty()) {
    return;
  }
  // Reading the whole chat decrypts every message: off the UI thread. Only the index comes back; the
  // decrypted messages die with this worker task.
  AppRuntime::PostWorkerNormal([this, facade, thread_id, generation]() {
    const auto started = std::chrono::steady_clock::now();
    std::vector<ThreadSearchEntry> index;
    size_t rows = 0;
    std::optional<std::string> failure;
    if (auto messages = facade->GetMessages(thread_id)) {
      rows = messages->size();
      index = BuildSearchIndex(*messages);
    } else {
      failure = messages.error().message;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    AppRuntime::PostUI([this, generation, index = std::move(index), rows, ms, failure]() mutable {
      if (failure) {
        // A failed read is not an empty chat: the index it would have replaced stays.
        log().warning << "chat search index failed err=" << *failure;
        if (search_.IndexFailed(generation, SteadyNowMs())) {
          ApplySearchView();
        }
      } else {
        log().info << "chat search index rows=" << rows << " ms=" << ms;
        // A late result (closed, or another chat since) is dropped here, before anything keeps it.
        if (search_.IndexReady(generation, std::move(index), SteadyNowMs())) {
          ApplySearchView();
          JumpToSearchHit();
        }
      }
      if (const std::optional<uint64_t> next = search_.Tick(SteadyNowMs())) {
        StartSearchIndex(*next);
      }
    });
  });
}

void ChatController::OnSearchMessagesChanged() {
  if (!search_.view().open) {
    return;
  }
  if (const std::optional<uint64_t> generation = search_.MessagesChanged(SteadyNowMs())) {
    StartSearchIndex(*generation);
  }
}

void ChatController::OnAgentTurnEnded(const std::string& thread_id) {
  if (!thread_id.empty() && thread_id == search_thread_) {
    OnSearchMessagesChanged(); // the answer is stored now
  }
  ask_ai_.OnTurnEnded(thread_id);
}

// --- the ask-AI panel (DEV/PLAN-ui-batch5-ask-ai.md 5a) ---------------------------------------------------

bool ChatController::AskAiCompact() const {
  return ChromeSnapshot().layout_mode == LayoutMode::Compact;
}

std::string ChatController::AskAiSharedThread(const bool create) {
  if (!messaging_ready_ || !facade_) {
    return {};
  }
  // The AI chat used last is the conversation Home, the panel and "AI chat" continue.
  std::string best;
  int64_t best_at = std::numeric_limits<int64_t>::min();
  if (auto threads = facade_->ListThreads()) {
    for (const Thread& thread : *threads) {
      if (thread.kind == ThreadKind::Ai && thread.listed && thread.updated_at > best_at) {
        best = thread.id;
        best_at = thread.updated_at;
      }
    }
  }
  if (best.empty() && create) {
    // Not opened: the page under the panel (maybe a chat with a person) stays as it is.
    if (auto created = facade_->CreateAiThread()) {
      best = created->id;
      SyncShellSessions();
    } else {
      log().warning << "Ask AI: could not create the AI chat: " << created.error().message;
    }
  }
  return best;
}

std::string ChatController::AskAiNewThread() {
  if (!messaging_ready_ || !facade_) {
    return {};
  }
  // Not opened: the page under the panel stays as it is.
  auto created = facade_->CreateAiThread();
  if (!created) {
    log().warning << "Ask AI: could not create the AI chat: " << created.error().message;
    return {};
  }
  SyncShellSessions();
  return created->id;
}

std::vector<AskAiRow> ChatController::AskAiRecentRows(const std::string& thread_id, const size_t limit) {
  std::vector<AskAiRow> out;
  if (!messaging_ready_ || !facade_ || thread_id.empty()) {
    return out;
  }
  auto page = facade_->GetMessagesPage(thread_id, std::nullopt, static_cast<int>(limit));
  if (!page || page->empty()) {
    return out;
  }
  int64_t oldest = page->front().display_order;
  for (const ThreadMessage& message : *page) {
    oldest = std::min(oldest, message.display_order);
  }
  for (const MessageDisplayRow& row : BuildMessageRows(thread_id, oldest, std::nullopt)) {
    if (row.content_rml.empty()) {
      continue;
    }
    out.push_back({row.message_id, row.row_class == "message-row-user", row.content_rml});
  }
  return out;
}

bool ChatController::AskAiLastIsQuestion(const std::string& thread_id) {
  if (!messaging_ready_ || !facade_ || thread_id.empty()) {
    return false;
  }
  auto newest = facade_->GetMessagesPage(thread_id, std::nullopt, 1);
  return newest && !newest->empty() && newest->back().sender_contact_id == kLocalSelfContactId;
}

std::string ChatController::AskAiStoreQuestion(const std::string& thread_id, const std::string& text) {
  if (!messaging_ready_ || !facade_) {
    return {};
  }
  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread_id;
  message.sender_contact_id = kLocalSelfContactId;
  message.text = text;
  message.timestamp = util::NowUnixMs();
  message.transport = MessageTransport::Local;
  auto stored = facade_->AppendMessage(message);
  if (!stored) {
    log().warning << "Ask AI: could not store the question: " << stored.error().message;
    return {};
  }
  (void)facade_->NameAiThreadFromFirstMessage(thread_id, text);
  (void)facade_->UpdatePreview(thread_id, text);
  SyncShellSessions();
  return stored->id.empty() ? message.id : stored->id;
}

int ChatController::AskAiPushSheet(const PaneSpec& spec, std::function<void()> on_closed) {
  if (!shell_navigation_.push_layer) {
    return -1;
  }
  const int layer_id = shell_navigation_.push_layer(spec);
  if (flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(layer_id, {}, std::move(on_closed));
  }
  return layer_id;
}

void ChatController::AskAiCloseSheet(const int layer_id) {
  if (layer_id >= 0 && shell_navigation_.close_layer) {
    shell_navigation_.close_layer(layer_id); // the coordinator hears it closing and tells the panel
  }
}

// --- the group info sheet (DEV/PLAN-ui-batch8-group-pages.md 1) -------------------------------------------------

void ChatController::OpenPeerSheet(ui::Event& ev) {
  if (messaging_ready_ && facade_ && chat_.show_peer_sheet) {
    auto thread = facade_->GetActiveThread();
    const bool group = thread && thread->kind == ThreadKind::Group && thread->group_id;
    if (ChooseChatHeaderSheet(AskAiCompact(), group) == ChatHeaderSheet::GroupInfo) {
      (void)group_info_.Open(thread->id);
      return;
    }
  }
  peer_sheet_.Open(ev);
}

GroupInfoScreen ChatController::GroupInfoRead(const std::string& thread_id) {
  return messaging_ready_ ? presenter_.GroupInfoOf(thread_id) : GroupInfoScreen{};
}

int ChatController::GroupInfoPushSheet(const PaneSpec& spec) {
  return shell_navigation_.push_layer ? shell_navigation_.push_layer(spec) : -1;
}

void ChatController::GroupInfoCloseSheet(const int layer_id) {
  if (shell_navigation_.close_layer && GroupInfoSheetOpen(layer_id)) {
    shell_navigation_.close_layer(layer_id);
  }
}

bool ChatController::GroupInfoSheetOpen(const int layer_id) const {
  return layer_id >= 0 && shell_navigation_.has_layer && shell_navigation_.has_layer(layer_id);
}

void ChatController::GroupInfoCall(const std::string& thread_id, const bool video) {
  if (call_actions_.start_call) {
    (void)call_actions_.start_call(thread_id, video); // joins the call that goes on, else the member picker
  }
}

void ChatController::GroupInfoMute(const std::string& thread_id, const bool muted) {
  menus_.OpenMute(thread_id, ui::Vector2i(120, 120), /*offer_unmute=*/muted); // an action sheet on the phone
}

void ChatController::GroupInfoRename(const std::string& thread_id, const bool for_everyone) {
  auto refresh = [this]() { group_info_.Refresh(); };
  if (for_everyone) {
    peer_sheet_.RenameForEveryone(thread_id, refresh);
  } else {
    peer_sheet_.RenameForMe(thread_id, refresh);
  }
}

void ChatController::GroupInfoRestoreName(const std::string& thread_id) {
  peer_sheet_.ClearMyName(thread_id);
  RefreshFromMessaging();
}

void ChatController::GroupInfoViewAll(const std::string& thread_id) {
  if (people_picker_notify_.open_group_members) {
    people_picker_notify_.open_group_members(thread_id);
  }
}

void ChatController::GroupInfoAddMember(const std::string& thread_id) {
  if (people_picker_notify_.open_group_add_members) {
    people_picker_notify_.open_group_add_members(thread_id);
  }
}

void ChatController::GroupInfoChoosePreset(const std::string& group_id, const GroupPreset current) {
  ChooseGroupPreset(group_id, current, ui::Vector2i(120, 120)); // an action sheet on the phone
}

void ChatController::ChooseGroupPreset(const std::string& group_id, const GroupPreset current,
                                       const ui::Vector2i position) {
  auto choose = [this, group_id](const GroupPreset preset) {
    if (auto set = presenter_.SetGroupPreset(group_id, preset); !set) {
      UserFeedback::Fail(set.error().message);
    }
    group_info_.Refresh();
    NotifySurfaceChanged();
  };
  std::vector<ContextMenuAction> actions;
  for (const GroupPreset preset : {GroupPreset::Open, GroupPreset::Managed}) {
    ContextMenuAction action;
    action.id = preset == GroupPreset::Managed ? "group_preset_managed" : "group_preset_open";
    action.label = Tr(preset == GroupPreset::Managed ? "group_info.preset_managed" : "group_info.preset_open");
    action.detail = Tr(preset == GroupPreset::Managed ? "group_info.preset_managed_detail" : "group_info.preset_open_detail");
    action.run = [choose, preset]() { choose(preset); };
    action.selected = preset == current;
    actions.push_back(std::move(action));
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void ChatController::GroupInfoTapMember(const std::string& thread_id, const std::string& identity) {
  if (people_picker_notify_.tap_group_member) {
    people_picker_notify_.tap_group_member(thread_id, identity);
  }
}

void ChatController::GroupInfoLeave(const std::string& thread_id) {
  OnCloseThread(thread_id); // asks first; an owner hands the group over or dissolves it
}

void ChatController::GroupInfoDissolve(const std::string& group_id) {
  DissolveGroupAfterConfirm(group_id);
}

void ChatController::GroupInfoInviteLink(const std::string& group_id) {
  group_link_->Open(group_id);
}

void ChatController::GroupInfoRevokeLink(const std::string& group_id, const std::string& link_id) {
  RevokeLinkAfterConfirm(group_id, link_id, [this](bool) { group_info_.Refresh(); });
}

void ChatController::RevokeLinkAfterConfirm(const std::string& group_id, const std::string& link_id,
                                            std::function<void(bool)> done) {
  ShowConfirm(
      Tr("group_link.revoke_title"), Tr("group_link.revoke_body"),
      [this, group_id, link_id, done = std::move(done)](const bool ok) {
        if (!ok) {
          done(false);
          return;
        }
        AppRuntime::PostWorkerAndReplyOnUI<Roe<void>>(
            WorkerLane::Normal, [this, group_id, link_id]() { return presenter_.RevokeInviteLink(group_id, link_id); },
            [done](Roe<void> revoked) {
              if (!revoked) {
                UserFeedback::Fail(revoked.error().message);
              }
              done(static_cast<bool>(revoked));
            });
      },
      /*destructive=*/true);
}

GroupLinkSheetPorts ChatController::MakeGroupLinkPorts() {
  GroupLinkSheetPorts ports;
  ports.push_sheet = [this](const PaneSpec& spec) { return GroupInfoPushSheet(spec); };
  ports.close_sheet = [this](const int layer_id) { GroupInfoCloseSheet(layer_id); };
  ports.sheet_open = [this](const int layer_id) { return GroupInfoSheetOpen(layer_id); };
  ports.load = [this](const std::string& group_id, std::function<void(Roe<GroupLinkRow>)> done) {
    AppRuntime::PostWorkerAndReplyOnUI<Roe<GroupLinkRow>>(
        WorkerLane::Normal, [this, group_id]() { return presenter_.InviteLinkFor(group_id); }, std::move(done));
  };
  ports.revoke = [this](const std::string& group_id, const std::string& link_id, std::function<void(bool)> done) {
    RevokeLinkAfterConfirm(group_id, link_id, std::move(done));
  };
  ports.copied = [this](const std::string& text) {
    if (ui::SystemInterface* system = ui::GetSystemInterface()) {
      system->SetClipboardText(text);
    }
    ShowToast(Tr("group_link.copied"));
  };
  ports.changed = [this]() { group_info_.Refresh(); };
  return ports;
}

GroupJoinPromptPorts ChatController::MakeJoinPromptPorts() {
  GroupJoinPromptPorts ports;
  ports.ask = [this](const std::string& link, std::function<void(Roe<std::string>)> done) {
    AppRuntime::PostWorkerAndReplyOnUI<Roe<std::string>>(
        WorkerLane::Normal,
        [this, link]() -> Roe<std::string> {
          return facade_ ? facade_->AskToJoinByLink(link) : Roe<std::string>(Error("Messaging is not ready"));
        },
        std::move(done));
  };
  ports.state = [this](const std::string& request_id) {
    return facade_ ? facade_->JoinByLinkState(request_id) : std::nullopt;
  };
  ports.confirm = [this](const std::string& request_id, std::function<void(Roe<Thread>)> done) {
    AppRuntime::PostWorkerAndReplyOnUI<Roe<Thread>>(
        WorkerLane::Normal,
        [this, request_id]() -> Roe<Thread> {
          return facade_ ? facade_->ConfirmJoinByLink(request_id) : Roe<Thread>(Error("Messaging is not ready"));
        },
        std::move(done));
  };
  ports.cancel = [this](const std::string& request_id) {
    if (facade_) {
      facade_->CancelJoinByLink(request_id);
    }
  };
  ports.show_confirm = [this](const std::string& title, const std::string& message, const std::string& ok_label,
                              std::function<void(bool)> done) {
    if (shell_feedback_.show_confirm) {
      shell_feedback_.show_confirm(title, message, std::move(done), ok_label);
    }
  };
  ports.toast = [this](const std::string& text) { ShowToast(text, ToastDuration::Long); };
  ports.open_thread = [this](const std::string& thread_id) {
    RefreshFromMessaging();
    ShowThreadInChat(thread_id);
  };
  ports.name_of = [this](const std::string& account) {
    return facade_ ? facade_->MemberPublicNameOf(account) : account;
  };
  return ports;
}

bool ChatController::OfferGroupJoin(const std::string& text) {
  return join_prompt_->Offer(text);
}

void ChatController::GroupLeaveConfirm(const std::string& message, std::function<void(bool ok)> done) {
  ShowConfirm(Tr("chat.group.leave_title"), message, std::move(done), /*destructive=*/true);
}

void ChatController::GroupLeaveHandOver(const std::string& thread_id, const std::string& group_id,
                                        const std::string& identity) {
  CloseChatWith(
      thread_id, [this, thread_id, group_id, identity]() { return presenter_.HandOverAndClose(thread_id, group_id, identity); },
      /*show_error_text=*/true);
}

void ChatController::ShowThreadInChat(const std::string& thread_id) {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  if (auto opened = facade_->OpenThread(thread_id); !opened) {
    log().warning << "Ask AI: could not open the AI chat: " << opened.error().message;
    return;
  }
  ShellSelectNavTab(NavTab::Sessions);
  ShellSetPrimaryPane("chat");
  FinalizeThreadDisplay();
  NotifySurfaceChanged();
}

void ChatController::AskAiOpenInChat(const std::string& thread_id) {
  // After the tap's own handler: the sheet and the page under it are rebuilt.
  AppRuntime::PostUI([this, thread_id]() { ShowThreadInChat(thread_id); });
}

void ChatController::AskAiSupport(const std::string& question) {
  AppRuntime::PostUI([this, question]() {
    if (!ask_support_) {
      UserFeedback::Fail(Tr("settings.support.unavailable"));
      return;
    }
    if (auto asked = ask_support_(question); !asked) {
      (void)UserFeedback::FailFrom(asked.error());
    }
  });
}

void ChatController::AskAiRunAction(const std::string& thread_id, const std::string& entry_id, const int action_index) {
  if (!messaging_ready_ || !facade_ || action_index < 0) {
    return;
  }
  auto found = facade_->GetMessage(thread_id, entry_id);
  if (!found || !found->has_value() || static_cast<size_t>(action_index) >= (*found)->chat_actions.size()) {
    log().warning << "Ask AI: answer button not found (index " << action_index << ")";
    return;
  }
  const TranscriptChatAction action = (*found)->chat_actions[static_cast<size_t>(action_index)];
  const auto payload = action.payload ? TryParseObject(*action.payload) : std::nullopt;
  const std::string type = payload ? payload->getString("type").value_or("") : std::string();
  // A permission card and a link are answered where they are; the session holding the card is found by its id.
  if (type == "tool_permission" || type == "open_url") {
    AppRuntime::PostUI([this, thread_id, action, entry_id]() {
      HandleLocalActionIn(thread_id, action.message, action.payload, entry_id); // a stale card is closed there
    });
    return;
  }
  // Anything else continues in the full AI chat, where its answer (a list, a form) can show.
  AppRuntime::PostUI([this, thread_id, action, entry_id]() {
    ask_ai_.Close();
    ShowThreadInChat(thread_id);
    HandleLocalActionIn(thread_id, action.message, action.payload, entry_id);
  });
}

bool ChatController::AskAiIntroSeen() const {
  return session_store_ && Store().Snapshot().profile_prefs.ai_panel_intro_seen;
}

void ChatController::AskAiMarkIntroSeen() {
  if (!session_store_) {
    return;
  }
  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  prefs.ai_panel_intro_seen = true;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
    log().warning << "Ask AI: could not remember the privacy card: " << saved.error().message;
  }
}

} // namespace pbr
