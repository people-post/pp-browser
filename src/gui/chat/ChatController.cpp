#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include "gui/chat/ChatController.h"
#include "gui/chat/ApprovalCard.h"
#include "gui/chat/AttachMenuRules.h"

#include "gui/chat/ActionNoticeText.h"
#include "gui/chat/DeliveryNoticeText.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/OutgoingAttachmentPrep.h"
#include "common/ImageMetadata.h"
#include "gui/AttachmentTypeLabel.h"
#include "gui/chat/VoiceBubble.h"
#include "gui/chat/VoiceSendPolicy.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_touch.h>
#include "lib/voice/VoiceSupport.h"
#include "foundation/platform/PickedFileName.h"
#include "gui/chat/AttachmentBackfillPolicy.h"
#include "gui/chat/ChatOpenMetrics.h"
#include "gui/chat/ChatPictureTextures.h"
#include "gui/UiFrameStats.h"
#include "foundation/diagnostics/OsCurrentMemory.h"
#include "common/Diag.h"
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
#include "gui/chat/AgentBusyRules.h"
#include "gui/chat/ReadWhenLooking.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/ui/DesktopWindowChrome.h"
#include "foundation/platform/ILocalNotifier.h"
#include "foundation/platform/PendingNotificationTap.h"
#include "foundation/platform/Platform.h"
#include "common/IPushDeviceRegistrar.h"
#include "foundation/platform/NativeFileDialog.h"
#include "lib/calls/CallControlFamily.h"
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
#include "domain/registration/BriefLlmKeyRenewal.h"
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
                    .jump_to_latest_count = chat_.jump_to_latest_count,
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
      chrome_(
          ChatThreadChrome::View{
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
          messaging_ready_, mesh_ready_) {
  redirectLogger("ChatController");
  VoiceComposerPorts voice_ports;
  voice_ports.limits = [this]() { return facade_ ? facade_->CurrentAttachmentLimits() : AttachmentLimits{}; };
  voice_ports.out_dir = [this]() {
    // blobs_view/voice-rec under a reserved name that is not a chat: wiped at start and when the vault locks.
    return facade_
               ? (std::filesystem::path(AttachmentViewRoot(facade_->ProfileDataDir(), kVoiceViewThread)) / "voice-rec")
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
           !chat_.attachment_uploading && !chat_.attachment_preparing &&
           !(chat_.attachment_failed && !chat_.attachment_in_transcript) && !ActiveThreadId().empty();
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
  playback_ports.resolve = [this](const std::string& message_id, std::function<void(std::optional<std::string>)> done) {
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
  // Measurement (chat.window): a page of older rows loaded; read the memory a second later, once they are drawn.
  scroller_.SetOnOlderLoaded([this]() { ScheduleChatWindowMetric(std::chrono::seconds(1), /*exited=*/false); });
  scroller_.SetDirtyTurns([this]() {
    // Paging and jumps rebuild the rows: the picture being sent goes back at the end (when the rows reach it).
    if (pending_uploads_.KeepAtTail(chat_.messages, ActiveThreadId(), !scroller_.LoadedMaxDisplayOrder())) {
      chat_.has_turns = true;
    }
    DirtyChatTurns();
    SyncPictureTextures(); // rows trimmed off the page: their pictures' textures go
  });
  working_set_.SetWidgetLookup(
      [this](const std::string& entry_id) -> const TurnWidgetState* { return widgets_.Find(entry_id); });
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
  chrome_.SetExpandLoadedMinFromOlderPage([this](const std::string& thread_id, int64_t before) {
    scroller_.ExpandLoadedMinFromOlderPage(thread_id, before);
  });
}

void ChatController::MakeReadyTasks() {
  ready_tasks_.reset();
  if (!facade_) {
    return;
  }
  // The three network ports run on a worker: they touch only the facade (its identity store and clients lock
  // themselves) and never this controller, the Store, preferences or UserFeedback. Everything else runs on the UI.
  ConversationsFacade* const facade = facade_;
  MessagingReadyTasks::Ports ports;
  ports.run_worker_then_ui = [](std::function<void()> work, std::function<void()> on_ui) {
    AppRuntime::PostWorkerAndReplyOnUI<int>(
        WorkerLane::Normal,
        [work = std::move(work)]() {
          work();
          return 0;
        },
        [on_ui = std::move(on_ui)](int) { on_ui(); });
  };
  ports.session = [this]() { return messaging_session_; };
  ports.still_ready = [this](const uint64_t started) { return started == messaging_session_ && MessagingReady(); };
  ports.register_push = [facade]() { (void)facade->SyncPushDevices(); };
  ports.auto_renew = [this]() { return Store().Snapshot().profile_prefs.auto_renew_registration; };
  ports.renew_network = [facade](const bool auto_renew) {
    RenewResult out;
    // The account this renewal runs for, read before the network: its result is stored only into that one.
    const std::optional<LocalIdentity> identity = facade->GetIdentity();
    if (!identity || identity->account_id.empty()) {
      out.error = Error("Messaging is not ready");
      return out;
    }
    out.account_id = identity->account_id;
    auto renewed = facade->MaybeAutoRenewRegistrationNetworkOnly(auto_renew);
    if (!renewed) {
      out.error = renewed.error();
    } else {
      out.registration = std::move(*renewed);
    }
    return out;
  };
  ports.renew_persist = [this](const RegistrationResult& registration, const std::string& account_id) -> Roe<void> {
    if (!facade_) {
      return Error("Messaging is not ready");
    }
    if (auto saved = facade_->PersistRegistrationRenewal(registration, account_id); !saved) {
      return saved.error();
    }
    return {};
  };
  ports.renew_hint = [this](const RenewResult& result, const bool auto_renew) {
    OnRecoveryRenewal(result, auto_renew);
    if (result.error) {
      log().warning << "Auto-renew registration failed: " << result.error->message;
    } else if (result.registration) {
      log().info << "Network registration auto-renewed";
    } else {
      const std::optional<LocalIdentity> identity = facade_ ? facade_->GetIdentity() : std::nullopt;
      // The reconnect row says it already when the relay does not know the account (B216).
      if (identity && ShouldRenewRegistration(*identity, 0, facade_->RegistrationBackendUrl()) && !auto_renew &&
          !recovery_.BannerVisible()) {
        UserFeedback::NeedsSetup(Tr("chat.hint.registration_expires"));
      }
    }
  };
  ports.brief_key_needed = [this]() {
    if (!facade_ || !MessagingReady()) {
      return false;
    }
    const std::optional<LocalIdentity> identity = facade_->GetIdentity();
    return identity && identity->brief_llm_api_key.empty() && identity->brief_llm_guest_api_key.empty();
  };
  ports.mint_brief_key_network = [](const std::string& llm_base_url) -> Roe<SecretText> {
    auto minted = MintBriefGuestLlmKey(llm_base_url);
    if (!minted) {
      return minted.error();
    }
    return std::move(minted->llm_api_key);
  };
  ports.account_id = [this]() {
    const std::optional<LocalIdentity> identity = facade_ ? facade_->GetIdentity() : std::nullopt;
    return identity ? identity->account_id : std::string();
  };
  ports.brief_key_persist = [this](const SecretText& key, const std::string& account_id) -> Roe<void> {
    if (!facade_) {
      return Error("Messaging is not ready");
    }
    // One modify of the guest key field only, into the account it was minted for; a key that came meanwhile (a
    // renewal brought a registered one, a 401 renewal stored a guest one) is kept.
    ConversationsFacade& facade = *facade_;
    const BriefLlmKeyStore store{
        .get = [&facade]() { return facade.GetIdentity(); },
        .modify = [&facade](const std::function<bool(LocalIdentity&)>& change) -> Roe<void> {
          if (auto changed = facade.ModifyLocalIdentity(change); !changed) {
            return changed.error();
          }
          return {};
        },
    };
    if (auto stored = StoreBriefGuestKeyIfNone(store, key, account_id); !stored) {
      return stored.error();
    }
    return {};
  };
  ports.brief_key_done = [this](const BriefKeyMintEnd& end) { agent_.OnBriefKeyMintEnded(end); };
  ports.after_identity_changed = [this]() {
    agent_.ReloadAgentConfig(); // runtime.llm.api_key takes the key now in the identity
    agent_.RefreshLlmSetupBanner();
  };
  ready_tasks_ = std::make_unique<MessagingReadyTasks>(std::move(ports), agent_.BriefKeyRun());
}

void ChatController::RunWhenRegistrationIdle(std::function<void()> run) {
  if (!run) {
    return;
  }
  if (ready_tasks_ && ready_tasks_->RenewInFlight()) {
    log().info << "registration waits for the automatic renewal in flight";
    UserFeedback::Note(Tr("chat.reconnect.please_wait"));
    ready_tasks_->AfterRenewal(std::move(run));
    return;
  }
  run();
}

void ChatController::OnRelayRegistrationSignal(const RelayRegistrationSignal signal) {
  const bool call_live = facade_ && facade_->AnyCallLive();
  switch (signal) {
    case RelayRegistrationSignal::NotRegistered: {
      const bool locked = !MessagingReady();
      const bool auto_renew = Store().Snapshot().profile_prefs.auto_renew_registration;
      const int64_t now_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
              .count();
      HandleNotRegisteredSignal(recovery_, locked, auto_renew, call_live, now_ms, MakeRecoveryHooks());
      break;
    }
    case RelayRegistrationSignal::PollOk: {
      const bool recovering = recovery_.CurrentState() != RegistrationRecovery::State::Ok;
      const RecoveryActions actions = recovery_.OnPollOk();
      send_fix_.OnPollOk(); // the update a failed message asked for got through: it is sent again
      if (recovering) {
        log().info << "registration recovery: the relay answers again";
        // The push binding sent at unlock went to a relay that did not know us: bind again, or the phone is not
        // woken for messages and calls until the next start.
        SyncPushDevices();
      }
      ApplyRecovery(actions);
      break;
    }
  }
}

void ChatController::OnRecoveryRenewal(const RenewResult& result, const bool auto_renew) {
  if (recovery_.CurrentState() == RegistrationRecovery::State::Ok) {
    // The routine renewal after unlock (also a backend switch, #602): not the recovery's, but it is an attempt.
    if (result.registration) {
      recovery_.OnRoutineRenewed(SteadyNowMs());
    }
    return;
  }
  RenewalOutcome outcome = RenewalOutcome::NothingDue;
  const char* result_name = "nothing_due";
  if (result.error) {
    const bool refused = IsRegistrationRefused(*result.error);
    outcome = refused ? RenewalOutcome::Refused : RenewalOutcome::Transient;
    result_name = refused ? "refused" : "transient";
  } else if (result.registration) {
    outcome = RenewalOutcome::Renewed;
    result_name = "ok";
  }
  log().info << "registration recovery: renewal result=" << result_name;
  switch (outcome) { // the metric takes words fixed at compile time only
    case RenewalOutcome::Renewed:
      MetricsLine("registration.reregister").Add("result", "ok").Emit();
      break;
    case RenewalOutcome::Refused:
      MetricsLine("registration.reregister").Add("result", "refused").Emit();
      break;
    case RenewalOutcome::Transient:
      MetricsLine("registration.reregister").Add("result", "transient").Emit();
      break;
    case RenewalOutcome::NothingDue:
      break;
  }
  if (outcome == RenewalOutcome::Renewed && facade_) {
    facade_->PublishAccountLockerSoon(); // as Me → Profile's register does: the locker keeps the relay binding
  }
  ApplyRecovery(recovery_.OnRenewal(outcome, auto_renew, facade_ && facade_->AnyCallLive()));
}

RecoveryHooks ChatController::MakeRecoveryHooks() {
  RecoveryHooks hooks;
  hooks.needs_user = [this]() {
    log().info << "registration recovery: needs the user";
    MetricsLine("registration.reregister").Add("result", "needs_user").Emit();
  };
  hooks.sync_banner = [this](const bool visible) {
    if (shell_.show_reconnect_banner != visible) {
      shell_.show_reconnect_banner = visible;
      DataModelHost::Instance().Dirty("shell", "show_reconnect_banner");
    }
  };
  hooks.mark_expired = [this]() {
    if (!facade_ || !ready_tasks_) {
      return false;
    }
    log().info << "registration recovery: the relay does not know this account; registering again";
    // What a 403 from the key rotation does too: the renewal then finds the registration due and registers.
    auto marked = facade_->ModifyLocalIdentity([](LocalIdentity& identity) {
      MarkRegistrationExpired(identity);
      return true;
    });
    if (!marked) {
      log().warning << "registration recovery: could not mark the registration expired: " << marked.error().message;
      return false;
    }
    return true;
  };
  hooks.run_renewal = [this]() { ready_tasks_->RenewRegistration(); };
  hooks.show_alert = [this]() {
    UserFeedback::Confirm(
        Tr("chat.reconnect.alert_title"), Tr("chat.reconnect.alert_body"),
        [this](const bool ok) {
          if (ok) {
            OnReconnectTapped();
          }
        },
        Tr("chat.reconnect.alert_ok"));
  };
  hooks.toast_reconnected = [this]() { ShowToast(Tr("chat.reconnect.done")); };
  return hooks;
}

void ChatController::ApplyRecovery(const RecoveryActions& actions) {
  ApplyRecoveryActions(actions, recovery_, MakeRecoveryHooks());
}

void ChatController::OnReconnectTapped() {
  if (!recovery_.OnUserTap()) {
    return; // connected again meanwhile
  }
  log().info << "registration recovery: the user asked to reconnect";
  if (reconnect_account_) {
    reconnect_account_({});
  }
}

void ChatController::ResetRecovery() {
  send_fix_.Reset(); // a failed message's pending resends go with the session
  recovery_.ResetSession();
  if (shell_.show_reconnect_banner) {
    shell_.show_reconnect_banner = false;
    DataModelHost::Instance().Dirty("shell", "show_reconnect_banner");
  }
}

void ChatController::SyncPushDevices() {
  if (ready_tasks_) {
    ready_tasks_->SyncPushDevices();
  }
}

void ChatController::RetryBriefKeyMint() {
  if (ready_tasks_) {
    ready_tasks_->RetryBriefKeyMint();
  }
}

void ChatController::MakeSendFixPorts() {
  SendFailureFix::Ports ports;
  ports.refresh_contact = [this](const std::string& contact_id, std::function<void(const ContactRefreshResult&)> done) {
    return refresh_contact_ && refresh_contact_(contact_id, std::move(done));
  };
  ports.resend = [this](const std::string& thread_id, const std::string& message_id) {
    if (!messaging_ready_ || !facade_) {
      return;
    }
    ApplySendMarkTap(thread_id, message_id, presenter_.ResendAfterCardUpdate(thread_id, message_id));
  };
  ports.update_my_card = [this](std::function<void(bool ok)> done) {
    if (!reconnect_account_) {
      done(false);
      return;
    }
    recovery_.OnUserTap(); // "Reconnected" is then said when the poll gets through, as for the row
    reconnect_account_(std::move(done));
  };
  ports.poll_ok_pending = [this]() { return recovery_.CurrentState() != RegistrationRecovery::State::Ok; };
  ports.now_ms = []() { return util::NowUnixMs(); };
  ports.progress = [](const std::string& text) { UserFeedback::Note(text); };
  ports.say = [this](const std::string& text) { ShowToast(text); };
  send_fix_.SetPorts(std::move(ports));
}

void ChatController::BindConversationsFacade(ConversationsFacade* facade) {
  facade_ = facade;
  send_fix_.Reset();
  MakeSendFixPorts();
  MakeReadyTasks();
  if (facade_) {
    // The server space is filling up: a plain line, said once per level (the hub counts).
    facade_->SetOnBlobSpaceWarning([this](const int percent) {
      ShowToast(Tr("chat.attachment.space_warning", {{"percent", std::to_string(percent)}}));
    });
    // The relay does not know this account (B216): register again, or ask (RegistrationRecovery).
    facade_->SetOnRelayRegistrationSignal(
        [this](const RelayRegistrationSignal signal) { OnRelayRegistrationSignal(signal); });
    // A picture's preview came (or cannot come): only the rows showing its placeholder are redrawn (B203).
    facade_->SetOnChatPreviewDone([this, alive = std::weak_ptr<int>(chat_window_alive_)](
                                      const std::string& thread_id, const std::vector<uint8_t>& hash, const bool made) {
      if (alive.lock()) {
        OnChatPreviewDone(thread_id, hash, made);
      }
    });
  }
  presenter_.BindFacade(facade);
  scroller_.BindConversationsFacade(facade);
  chrome_.BindConversationsFacade(facade);
  scroller_.SetRowBuilder(
      [this](const std::string& thread_id, std::optional<int64_t> oldest, std::optional<int64_t> newest) {
        return BuildMessageRows(thread_id, oldest, newest);
      });
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
    const std::string reason = std::string(slash == std::string_view::npos ? file : file.substr(slash + 1)) + ":" +
                               std::to_string(where.line());
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
  // The chat before (or this one, opened again): its preview requests not started go; this open asks for its own.
  FinishPreviewsProbe();
  preview_batcher_.Clear();
  if (facade_) {
    facade_->CancelChatPreviews();
  }
  // Measurement (chat.open): this open's display sync and the SyncLayout after it are the ones timed.
  open_probe_ = ChatOpenProbe{};
  open_probe_.armed = true;
  ++chat_exit_turn_; // back in a chat: a pending "left the chat" reading is void
  chat_showing_ = true;
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

void ChatController::OnAttentiveAgain() {
  if (!messaging_ready_) {
    return;
  }
  // Refresh only when something was marked: a focus gain with nothing to read must stay cheap.
  if (MarkOpenChatReadIfLooking()) {
    RefreshFromMessaging();
  }
}

bool ChatController::MarkOpenChatReadIfLooking() {
  if (reading_open_chat_ || !facade_) {
    return false;
  }
  const std::string thread_id = ActiveThreadId(); // a copy: MarkRead may reach the facade's own string
  bool has_unread = false;
  if (!thread_id.empty()) {
    if (auto found = facade_->GetThread(thread_id); found && *found) {
      has_unread = (*found)->unread_count > 0;
    }
  }
  // A notification tap waiting to be routed (the app came to the front for it) opens another chat next: this one
  // keeps its unread until it is shown again. Peeked, never taken.
  PendingNotificationTap& tap = PendingNotificationTap::Instance();
  const auto now = PendingNotificationTap::Clock::now();
  const bool route_unknown_thread = !tap.FreshHint(now).empty() || tap.PeekCallRoute(now);
  const bool tap_elsewhere = TapPendingElsewhere(tap.PeekThread(), route_unknown_thread, thread_id);
  if (!ShouldMarkOpenChatRead(AppLifecycle::IsUserAttentive(), !thread_id.empty(),
                              ChatTranscriptOnScreen(ChromeSnapshot(), thread_id, thread_id), has_unread,
                              tap_elsewhere)) {
    return false;
  }
  reading_open_chat_ = true; // MarkRead -> messages changed -> RefreshFromMessaging -> here: stops at this guard
  presenter_.MarkRead(thread_id);
  ILocalNotifier::Instance().ClearForThread(thread_id);
  reading_open_chat_ = false;
  return true;
}

void ChatController::OnSelectThread(std::string thread_id) { // by value: ShellSelectNavTab below rebuilds Home (B161)
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
    mentions_.SyncThread(); // a group chat: its @ lines are read now, before anyone types `@`
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
  if (picks_.State().serial != 0 && picks_.State().thread_id == thread_id) {
    AbandonPick(); // a pick of the deleted chat: its late results are dropped and released
  }
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
    ShowConfirm(
        Tr(title_key), Tr(body_key),
        [run = std::move(run)](const bool ok) {
          if (ok) {
            run();
          }
        },
        true);
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
        CloseChatWith(thread_id,
                      [this, thread_id, group_id]() { return presenter_.DismissGroupAndClose(thread_id, group_id); });
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
        CloseChatWith(thread_id,
                      [this, thread_id, group_id]() { return presenter_.LeaveGroupAndClose(thread_id, group_id); });
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
                         label =
                             (*contact)->display_name.empty() ? (*contact)->server_nickname : (*contact)->display_name;
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
  ShowConfirm(
      Tr("chat.group.dissolve_title"), Tr("chat.group.dissolve_confirm"),
      [this, group_id](const bool ok) {
        if (!ok) {
          return;
        }
        // The chat stays for everyone, read-only: nothing is closed, the screen only refreshes.
        if (auto dissolved = presenter_.DissolveGroup(group_id); !dissolved) {
          UserFeedback::Fail(dissolved.error().message);
        }
        RefreshFromMessaging();
      },
      true);
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

void ChatController::OnAiSessionsSearch(ui::Event& ev, const bool submit) {
  // A change event runs before data-value copies the field into the model: take the new text from the event.
  if (!submit) {
    shell_.ai_sessions_query = ev.GetParameter<ui::String>("value", shell_.ai_sessions_query);
  }
  ApplySessionList();
  DataModelHost::Instance().Dirty("shell", "ai_sessions");
  if (submit || ev.GetParameter<bool>("linebreak", false)) {
    if (ui::Context* context = ev.GetTargetElement() ? ev.GetTargetElement()->GetContext() : nullptr) {
      if (ui::Element* focused = context->GetFocusElement()) {
        focused->Blur();
      }
    }
    if (ui::SystemInterface* system = ui::GetSystemInterface()) {
      system->DeactivateKeyboard();
    }
  }
}

bool ChatController::SessionRowMatches(const std::string& title, const std::string& preview,
                                       const std::string& lowered_query) {
  return lowered_query.empty() || util::ToLowerAscii(title).find(lowered_query) != std::string::npos ||
         util::ToLowerAscii(preview).find(lowered_query) != std::string::npos;
}

void ChatController::OnToggleArchivedAi() {
  ai_session_window_.Reset(); // the other list starts at its first rows
  presenter_.ToggleArchivedAi();
  RefreshFromMessaging();
  NotifySurfaceChanged();
}

void ChatController::OnToggleArchived() {
  session_window_.Reset(); // the other list starts at its first rows
  presenter_.ToggleArchived();
  RefreshFromMessaging();
  NotifySurfaceChanged();
}

void ChatController::OnClearHistory() {
  ClearHistoryOf(ActiveThreadId());
}

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

  ShowConfirm(
      Tr("chat.clear_history"), message,
      [this, thread_id, is_ai](bool ok) {
        if (!ok) {
          return;
        }
        if (!facade_->ClearThreadHistory(thread_id, /*forget_memory=*/is_ai)) {
          return;
        }
        presenter_.ForgetCachedPrefs(); // clearing also resets the chat's "don't ask again" for the AI
        AfterHistoryCleared(thread_id);
      },
      true);
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

  ShowConfirm(
      Tr("chat.forget_memory.title"), Tr("chat.forget_memory.body"),
      [this, thread_id](bool ok) {
        if (!ok) {
          return;
        }
        if (!facade_->ForgetThreadMemory(thread_id)) {
          return;
        }
        NotifySurfaceChanged();
      },
      true);
}

void ChatController::RefreshFromMessaging() {
  MarkOpenChatReadIfLooking(); // first, so the rest of the refresh draws the read state
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
  mentions_.SyncThread(); // the open group's @ lines: reloaded when its members or names may have changed
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
  CloseSearch();                                       // nothing decrypted from the old profile stays in memory
  QueueTextureRelease(picture_textures_.UnmountAll()); // the old profile's pictures leave video memory
  vault_locked_seen_ = false;
  preview_batcher_.Clear();
  previews_probe_ = ChatPreviewsProbe{};
  pending_uploads_.Drop(); // the lock check never sees this teardown: the plaintext thumbnail goes here
  AbandonPick();
  video_bubble_thread_.clear();
  messaging_ready_ = false;
  ++messaging_session_; // a task result of the old profile never reaches the new one
  if (ready_tasks_) {
    ready_tasks_->OnMessagingTornDown(); // the teardown dropped work still queued: free its runs
  }
  ResetRecovery();
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
  ai_session_window_.Reset();
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
  presenter_.Refresh(); // ApplySessionList copies it in
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

} // namespace

// One chats-list row as the RML binds it. Both texts are bound as text ({{session.title}}), so names and message text
// need no escaping here.
SessionDisplayRow ToSessionDisplayRow(const ChatSessionRow& from) {
  SessionDisplayRow row;
  row.id = from.id.c_str();
  row.title = from.title_is_default_ai ? Tr("chat.new_chat").c_str() : from.title.c_str();
  row.preview = from.preview.c_str();
  row.preview_icon = from.preview_icon.c_str();
  row.preview_empty = from.preview_empty;
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
    row.preview_icon = "";
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
    if (searching && !SessionRowMatches(row.title.c_str(), from.preview_empty ? std::string() : from.preview, query)) {
      continue;
    }
    shell_.sessions.push_back(std::move(row));
  }
  // AI conversations for the AI drawer, newest/pinned first: the chats list's window (20 rows, 20 more near the end;
  // Kenneth 2026-11-07), the open one always drawn; its search box filters every conversation.
  for (size_t i = 0; i < list.ai_rows.size(); ++i) {
    if (list.ai_rows[i].active) {
      ai_session_window_.Include(i);
      break;
    }
  }
  const std::string ai_query = LowerAscii(shell_.ai_sessions_query.c_str());
  const size_t ai_visible = ai_query.empty() ? ai_session_window_.Visible(list.ai_rows.size()) : list.ai_rows.size();
  shell_.ai_sessions.clear();
  shell_.ai_sessions.reserve(ai_visible);
  for (size_t i = 0; i < ai_visible; ++i) {
    const ChatSessionRow& from = list.ai_rows[i];
    SessionRow row = ToSessionDisplayRow(from);
    if (SessionRowMatches(row.title.c_str(), from.preview_empty ? std::string() : from.preview, ai_query)) {
      shell_.ai_sessions.push_back(std::move(row));
    }
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
  shell_.home_mentions_where = home_summary_.mentions.preview.empty()
                                   ? home_summary_.mentions.title.c_str()
                                   : (home_summary_.mentions.title + " · " + home_summary_.mentions.preview).c_str();
  shell_.home_new_people = home_summary_.new_contacts;
  shell_.home_security_ok = session_store_ && Store().Snapshot().profile_prefs.recovery_phrase_seen;
  // The widgets page is off until it gets its own page: the stored preference is kept but never read here.
  constexpr bool kWidgetsPageOnHome = false;
  shell_.home_show_dynamic = kWidgetsPageOnHome && session_store_ && Store().Snapshot().profile_prefs.home_show_dynamic;
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
  const std::vector<std::string> starred =
      session_store_ ? Store().Snapshot().profile_prefs.starred_contacts : std::vector<std::string>{};
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
  const HomeCalls calls =
      layout.Kept(kHomeWidgetCalls) ? SummarizeHomeCalls(home_calls_, LocalMidnightMs(now_ms), now_ms) : HomeCalls{};
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
      rows[i]->Animate("transform", HomeEditRowTransform(0.f, 1.f), kHomeEditSlideSeconds, kHomeEditEase, 1, false, 0.f,
                       &from);
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
  if (ChromeSnapshot().layout_mode != LayoutMode::Compact || ChromeSnapshot().nav_tab != NavTab::Home ||
      !HomeLayout().Kept(kHomeWidgetCalls) || !call_actions_.recent_calls_reader) {
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

void ChatController::OpenReactors(const std::string& message_id) {
  if (!messaging_ready_ || reactors_layer_ >= 0 || !shell_navigation_.push_layer) {
    return;
  }
  const std::vector<ReactorRow> reactors = presenter_.ReactorsOf(message_id);
  if (reactors.empty()) {
    return;
  }
  chat_.reactor_rows.clear();
  for (const ReactorRow& reactor : reactors) {
    ReactorDisplayRow row;
    row.emoji = reactor.emoji.c_str();
    row.title = reactor.title.c_str();
    const AvatarGlyph glyph = MakeAvatarGlyph(reactor.title, reactor.stable_id);
    row.avatar_letter = glyph.letter.c_str();
    row.avatar_tone = glyph.tone;
    row.has_icon = !reactor.icon_path.empty();
    row.icon_src = reactor.icon_path.c_str();
    row.is_self = reactor.is_self;
    chat_.reactor_rows.push_back(std::move(row));
  }
  reactors_message_id_ = message_id;
  DataModelHost::Instance().Dirty("chat", "reactor_rows");
  PaneSpec spec;
  spec.key = "reactors_sheet";
  spec.page_sheet = true;
  reactors_layer_ = shell_navigation_.push_layer(spec);
  if (reactors_layer_ >= 0 && flow_coordinator_.begin_modal) {
    flow_coordinator_.begin_modal(reactors_layer_, {}, [this]() {
      reactors_layer_ = -1;
      reactors_message_id_.clear();
      chat_.reactor_rows.clear();
      DataModelHost::Instance().Dirty("chat", "reactor_rows");
    });
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

void ChatController::OnAiSessionsScroll() {
  ui::Element* el = AiSessionListElement();
  if (!el || !shell_.ai_sessions_query.empty()) {
    return;
  }
  const bool near_bottom = SessionListNearBottom(el->GetScrollTop(), el->GetScrollHeight(), el->GetClientHeight());
  if (ai_session_window_.ExtendIfNearBottom(presenter_.List().ai_rows.size(), near_bottom)) {
    ApplySessionList();
    DataModelHost::Instance().Dirty("shell", "ai_sessions");
  }
}

void ChatController::FillSessionListAfterLayout() {
  // Rows that do not reach the bottom of the list cannot be scrolled, so the scroll event never asks for more:
  // after a layout, grow the window until the list overflows (or every row is drawn). One step per layout.
  if (shell_.sessions_more) {
    if (ui::Element* el = SessionListElement();
        el && SessionListNeedsFill(el->GetScrollHeight(), el->GetClientHeight())) {
      OnSessionsLoadMore();
    }
  }
  if (shell_.ai_sessions_query.empty() && ai_session_window_.More(presenter_.List().ai_rows.size())) {
    if (ui::Element* el = AiSessionListElement(); el &&
                                                  SessionListNeedsFill(el->GetScrollHeight(), el->GetClientHeight()) &&
                                                  ai_session_window_.Extend(presenter_.List().ai_rows.size())) {
      ApplySessionList();
      DataModelHost::Instance().Dirty("shell", "ai_sessions");
    }
  }
}

ui::Element* ChatController::AiSessionListElement() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  for (const char* id : {"ai-drawer-sessions", "home-ai-sessions"}) {
    if (ui::Element* el = doc->GetElementById(id); el && el->GetClientHeight() > 0.5f) {
      return el; // the one laid out (the desktop list stays in the DOM, hidden, on a phone)
    }
  }
  return nullptr;
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
  ShowConfirm(Tr("chat.device_lock.confirm_title"), Tr("chat.device_lock.confirm_body"), [this, thread_id](bool ok) {
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
  const bool show = chat_.show_attach_button &&
                    ShowCaptureEntry(Platform::IsMobile(), facade_ && facade_->AnyCallLive(), InAiComposerContext(),
                                     chat_.attachment_uploading || chat_.attachment_preparing ||
                                         (chat_.attachment_failed && !chat_.attachment_in_transcript));
  if (chat_.show_capture != show) {
    chat_.show_capture = show;
    DataModelHost::Instance().Dirty("chat", "show_capture");
  }
}

void ChatController::ToggleVoiceMode() {
  if (chat_.voice_recording || (!chat_.voice_mode && (!chat_.show_voice || chat_.editing_message))) {
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
  RefreshAttachmentInTranscript();
}

void ChatController::RefreshAttachmentInTranscript() {
  // A video's bubble is on its way (its poster is being read): the card stays hidden meanwhile too.
  const bool shown = pending_uploads_.Queue().ShowsIn(ActiveThreadId()) ||
                     (!video_bubble_thread_.empty() && video_bubble_thread_ == ActiveThreadId());
  if (chat_.attachment_in_transcript != shown) {
    chat_.attachment_in_transcript = shown;
    DataModelHost::Instance().Dirty("chat", "attachment_in_transcript");
  }
}

void ChatController::ApplyPendingUpload(PendingUploadStep step, const bool redraw) {
  const std::string thread_id =
      !step.next.thread_id.empty() ? step.next.thread_id : pending_uploads_.Upload().thread_id;
  const bool changed = pending_uploads_.Apply(std::move(step));
  SyncComposerInputState(); // the bubble decides whether the input stays typeable (attachment_in_transcript)
  if (changed && redraw && messaging_ready_ && !thread_id.empty() && thread_id == ActiveThreadId()) {
    SyncDisplayFromThread(); // the row drawn again (or gone) with the transcript's own rules
    DirtyChatTurns();
  }
}

void ChatController::SyncComposerInputState() {
  RefreshAttachmentHere();
  // The chip and the locked input belong to the chat the file is being sent in; the other chats stay typeable. With a
  // picture bubble in the transcript text stays typeable; only another attachment waits (ComposerLocksOf).
  const ComposerLocks locks =
      ComposerLocksOf(chat_.compose_disabled, chat_.attachment_here, chat_.attachment_in_transcript,
                      chat_.attachment_uploading || chat_.attachment_preparing, chat_.image_preparing,
                      chat_.voice_recording, chat_.editing_message);
  if (chat_.composer_input_disabled != locks.input || chat_.attach_disabled != locks.attach) {
    chat_.composer_input_disabled = locks.input;
    chat_.attach_disabled = locks.attach;
    DataModelHost::Instance().Dirty("chat", "composer_input_disabled");
    DataModelHost::Instance().Dirty("chat", "attach_disabled");
  }
  // Whether a voice message can be sent depends on the same upload state: a toggle that went away while a file
  // was being sent must come back when the upload ends, not at the next chrome update.
  if (voice_) {
    voice_->Refresh();
  }
  // A send preparing or in flight hides the microphone (VoiceComposer eligible): it stays in its place, greyed.
  const bool voice_off = (chat_.attachment_uploading || chat_.attachment_preparing) && !chat_.show_voice &&
                         VoiceMessagesSupported() && chat_.show_attach_button && !InAiComposerContext();
  if (chat_.voice_entry_off != voice_off) {
    chat_.voice_entry_off = voice_off;
    DataModelHost::Instance().Dirty("chat", "voice_entry_off");
  }
  RefreshShowCapture();
  RefreshPickOverlay(); // the overlay belongs to the chat its pick started in
}

uint64_t ChatController::ArmPick() {
  const uint64_t serial = picks_.Arm(ActiveThreadId());
  RefreshPickOverlay();
  return serial;
}

void ChatController::ShowPick(const uint64_t serial) {
  if (!picks_.IsCurrent(serial) || picks_.State().showing) {
    return;
  }
  picks_.Show(serial, util::NowUnixMs());
  RefreshPickOverlay();
  // The second line after kPickSlowMs, only for this pick and only while its overlay still shows.
  AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kPickSlowMs),
                                         [this, serial, alive = std::weak_ptr<int>(send_mark_alive_)]() {
                                           AppRuntime::PostUI([this, serial, alive]() {
                                             if (alive.lock() && picks_.IsCurrent(serial)) {
                                               RefreshPickOverlay();
                                             }
                                           });
                                         });
}

void ChatController::EndPick(const uint64_t serial) {
  if (!picks_.IsCurrent(serial)) {
    return;
  }
  picks_.End(serial);
  RefreshPickOverlay();
}

std::optional<std::string> ChatController::TakePickResult(const uint64_t serial,
                                                          const std::vector<std::string>& files) {
  // Only to the chat the pick was armed in, and only while it is open (never ActiveThreadId() of later).
  const bool left_chat = picks_.LeftChat(serial, ActiveThreadId());
  std::optional<std::string> thread =
      picks_.TakeResult(serial, ActiveThreadId(), files, [this](const std::string& path) { release_pick_(path); });
  if (!thread && left_chat &&
      std::any_of(files.begin(), files.end(), [](const std::string& f) { return !f.empty(); })) {
    // Never dropped without a word. A camera shot is only our temp copy (iOS tmp/pp-picks, Android cache/capture):
    // it was deleted with the rest, so no plaintext stays behind.
    UserFeedback::Fail(Tr("chat.attach.not_sent_left_chat"));
  }
  if (!thread && picks_.State().serial == 0) {
    EndAttachmentPreparing(); // no pick is waiting any more: the entries come back
    RefreshPickOverlay();
  }
  return thread;
}

void ChatController::AbandonPick() {
  picks_.EndCurrent();
  EndAttachmentPreparing();
  RefreshPickOverlay();
}

void ChatController::RefreshPickOverlay() {
  const PickOverlay overlay = PickOverlayOf(picks_.State(), ActiveThreadId(), /*choice_shown=*/false,
                                            /*bubble_built=*/false, util::NowUnixMs());
  const bool busy = overlay != PickOverlay::Hidden;
  const bool slow = overlay == PickOverlay::TwoLines;
  if (chat_.picking_busy != busy || chat_.picking_busy_slow != slow) {
    chat_.picking_busy = busy;
    chat_.picking_busy_slow = slow;
    DataModelHost::Instance().Dirty("chat", "picking_busy");
    DataModelHost::Instance().Dirty("chat", "picking_busy_slow");
  }
}

void ChatController::OnCancelPicking() {
  if (picks_.State().serial == 0) {
    return;
  }
  // A video waiting for its poster is a send already: it stops, and the screen is free at once (the worker's late
  // poster and preparation are dropped: their send is no longer the current one).
  if (!video_bubble_thread_.empty() && send_cancel_) {
    send_cancel_->store(true);
    send_cancel_.reset();
    video_bubble_thread_.clear();
    chat_.attachment_uploading = false;
    ResetUploadCard(std::string());
    chat_.attachment_draft_name = "";
    chat_.attachment_status = "";
    chat_.status = "";
    DirtyChatChrome();
    DirtyChatHeader();
  }
  AbandonPick(); // what the copy or the poster brings back later is dropped and released
  SyncComposerInputState();
}

void ChatController::OnAttachFile(ui::Event* ev) {
  if (!messaging_ready_ || !facade_) {
    return;
  }
  // One attachment at a time; beside a failed bubble a new one may start (the bubble gives way, its staged copy stays
  // for the chip; the bubble kept its own record id, so no Retry sends the wrong file).
  if (chat_.composer_input_disabled || chat_.attachment_uploading || chat_.attachment_preparing ||
      !chat_.show_attach_button || chat_.editing_message) {
    return;
  }
  // B90: the menu opens at the attach button, not at a fixed corner (which was behind the keyboard on a phone).
  if (ev) {
    const ui::Vector2i anchor = MenuPositionAboveEvent(*ev);
    attach_menu_x_ = anchor.x;
    attach_menu_y_ = anchor.y;
  }
  if (InAiComposerContext()) {
    // A phone's AI conversation (Kenneth 2026-11-07): the bold plus opens the Home composer's floating card above it.
    if (ev && Platform::IsMobile() && ChromeSnapshot().layout_mode == LayoutMode::Compact) {
      agent_.ShowHomeAttachMenu(ev->GetCurrentElement(), /*above=*/true);
      return;
    }
    agent_.OnAttachAiImage();
    return;
  }
  const std::string thread_id = ActiveThreadId();
  if (thread_id.empty()) {
    return;
  }

  // Where the file came from decides what is done to a picture (see OutgoingPickSource). Each picker opening is a pick
  // with its own serial (AttachmentPicks): a result of a pick cancelled or replaced meanwhile is dropped, its copy freed.
  const auto picked = [this](const OutgoingPickSource source, const uint64_t serial) {
    return [this, source, serial](std::vector<std::string> paths) {
      AppRuntime::PostUI([this, source, serial, paths = std::move(paths)]() mutable {
        const std::optional<std::string> thread = TakePickResult(serial, paths);
        if (!thread) {
          return;
        }
        EndAttachmentPreparing();
        if (paths.empty()) {
          EndPick(serial); // the picker was cancelled
          return;
        }
        StartAttachmentUpload(
            std::move(paths.front()), source, /*voice_file=*/false, *thread,
            source == OutgoingPickSource::File ? std::optional(PendingUploadKind::File) : std::nullopt, serial);
      });
    };
  };
  // A picture is asked about before it goes: sent smaller (the default) or as it is. A video from the phone's
  // library has no such choice and goes straight on.
  const auto picked_picture = [this](const uint64_t serial) {
    return [this, serial](std::vector<std::string> paths) {
      AppRuntime::PostUI([this, serial, paths = std::move(paths)]() mutable {
        const std::optional<std::string> thread = TakePickResult(serial, paths);
        if (!thread) {
          return;
        }
        EndAttachmentPreparing();
        if (paths.empty()) {
          EndPick(serial);
          return;
        }
        std::string path = std::move(paths.front());
        if (AttachmentMimeFromFilename(path).rfind("video/", 0) == 0) {
          StartAttachmentUpload(std::move(path), OutgoingPickSource::Photo, /*voice_file=*/false, *thread,
                                PendingUploadKind::Video, serial);
          return;
        }
        ShowPictureSendChoice(std::move(path), *thread);
      });
    };
  };
  // "Photo" on a phone: the library with pictures and videos.
  const auto pick_photo_or_video = [this, picked_picture]() {
    return [this, picked_picture]() {
      const uint64_t serial = ArmPick();
      ShowOpenPhotoLibraryDialog(Backend::GetWindow(), picked_picture(serial), [this, serial](const std::string& name) {
        AppRuntime::PostUI([this, serial, name]() {
          if (!picks_.IsCurrent(serial)) {
            return; // cancelled or replaced meanwhile: nothing is waited for
          }
          BeginAttachmentPreparing(name);
          ShowPick(serial); // the copy out of the library runs: the overlay until the choice or the bubble
        });
      });
    };
  };
  // "Picture" on the desktop: the image-file dialog.
  const auto pick_image = [this, picked_picture]() {
    return [this, picked_picture]() { ShowOpenImageFileDialog(Backend::GetWindow(), picked_picture(ArmPick())); };
  };
  const auto pick_file = [this, picked]() {
    return [this, picked]() { ShowOpenFileDialog(Backend::GetWindow(), picked(OutgoingPickSource::File, ArmPick())); };
  };

  // A phone's chat (Kenneth 2026-11-07): the bold plus opens the Home composer's floating card over it, Camera / Photos /
  // File, each the chat's own path (the camera capture, the library with the send-smaller choice, the file picker).
  if (ev && Platform::IsMobile() && ChromeSnapshot().layout_mode == LayoutMode::Compact) {
    ContextMenuHost::Instance().ShowActionsPopoverLeftAligned(
        ev->GetCurrentElement(),
        AgentTurnController::HomeAttachActions([this]() { OnAttachCapture(nullptr); }, pick_photo_or_video(),
                                               pick_file()),
        {}, /*above=*/true);
    return;
  }
  // A phone keeps photos and files apart; the desktop dialog shows both at once, so there a picture and a file
  // have their own entries.
  std::vector<ContextMenuAction> actions;
  if (Platform::IsMobile()) {
    actions.push_back(
        {"attach_photo", Tr("chat.attach.photo"), nullptr, pick_photo_or_video(), "../icons/v2/photo.svg"});
  } else {
    actions.push_back({"attach_picture", Tr("chat.attach.picture"), nullptr, pick_image()});
  }
  actions.push_back({"attach_file", Tr("chat.attach.file"), nullptr, pick_file(), "../icons/v2/file.svg"});
  actions.push_back(AttachCancelAction());
  ContextMenuHost::Instance().ShowActionsAbove(ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions), {},
                                               Tr("chat.attach.title"));
}

void ChatController::ShowPictureSendChoice(std::string path, const std::string& thread_id) {
  picks_.EndCurrent(); // the choice is up: the wait is over
  RefreshPickOverlay();
  // Only one of the three runs; Cancel releases the file (the two sends hand it to StartAttachmentUpload). A sheet
  // closed without choosing (tap beside it, Escape) releases it too, so no private copy outlives the choice.
  auto release = [this](const std::string& picture) { release_pick_(picture); };
  auto actions = PictureChoiceActions(
      path,
      [this, thread_id](const std::string& picture, const bool original) {
        if (thread_id.empty() || thread_id != ActiveThreadId()) {
          release_pick_(picture); // its chat is no longer the open one: nothing is sent
          UserFeedback::Fail(Tr("chat.attach.not_sent_left_chat"));
          return;
        }
        StartAttachmentUpload(picture, original ? OutgoingPickSource::Original : OutgoingPickSource::Photo,
                              /*voice_file=*/false, thread_id, PendingUploadKind::Picture);
      },
      release, Tr("chat.attach.send_compressed"), Tr("chat.attach.send_original"), Tr("common.cancel"));
  ContextMenuHost::Instance().ShowActionsAbove(
      ui::Vector2i(attach_menu_x_, attach_menu_y_), std::move(actions),
      [release, picture = std::move(path)]() { release(picture); }, Tr("chat.attach.send_photo_title"));
}

void ChatController::OnAttachCapture(ui::Event* ev) {
  if (!messaging_ready_ || !facade_ || !chat_.show_capture || chat_.composer_input_disabled ||
      chat_.attachment_uploading || chat_.attachment_preparing || InAiComposerContext() || ActiveThreadId().empty() ||
      chat_.editing_message) {
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
  const uint64_t serial = ArmPick();
  ShowCameraCapture(
      Backend::GetWindow(), kind, limits.video_max_seconds, limits.max_video_bytes,
      [this, serial](const CameraCaptureOutcome outcome, std::vector<std::string> paths) {
        AppRuntime::PostUI([this, serial, outcome, paths = std::move(paths)]() mutable {
          const std::optional<std::string> thread = TakePickResult(serial, paths);
          if (!thread) {
            return;
          }
          EndAttachmentPreparing();
          HandleCaptureResult(MakeCaptureFlowPorts(serial, *thread), outcome, std::move(paths));
          if (video_bubble_thread_.empty()) {
            EndPick(serial); // no video waiting for its poster: the choice showed, or it failed / was cancelled
          }
        });
      },
      [this, serial](const std::string& name) {
        AppRuntime::PostUI([this, serial, name]() {
          if (!picks_.IsCurrent(serial)) {
            return;
          }
          BeginAttachmentPreparing(name);
          ShowPick(serial);
        });
      });
}

CaptureFlowPorts ChatController::MakeCaptureFlowPorts(const uint64_t pick_serial, const std::string& thread_id) {
  CaptureFlowPorts ports;
  ports.release = [this](const std::string& path) { release_pick_(path); };
  ports.ask_picture = [this, thread_id](std::string path) { ShowPictureSendChoice(std::move(path), thread_id); };
  ports.send_video = [this, pick_serial, thread_id](std::string path) {
    StartAttachmentUpload(path, OutgoingPickSource::Photo, /*voice_file=*/false, thread_id, PendingUploadKind::Video,
                          pick_serial);
  };
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

std::function<bool(uint64_t done, uint64_t total)> ChatController::MakeSendProgress(
    std::shared_ptr<std::atomic<bool>> offline_stop) {
  const uint64_t stay_hint_bytes = facade_ ? facade_->CurrentAttachmentLimits().stay_hint_bytes : 0;
  auto last_percent = std::make_shared<std::atomic<int>>(-1);
  ConversationsFacade* const facade = facade_;
  struct Held {
    std::mutex mutex;
    OfflineHold hold;
  };
  auto held = std::make_shared<Held>();
  return [this, last_percent, stay_hint_bytes, facade, held, offline_stop](const uint64_t done, const uint64_t total) {
    // Offline for kOfflineStopMs without a break (no link under any tunnel): the transfer stops at its next tick
    // instead of waiting out the network timeout (30 s on the iPhone); the staged copy stays, so the bubble turns to
    // Retry. A short drop (a Wi-Fi / cellular handover) never stops it: the transfer's own retries carry on.
    if (facade) {
      bool stop = false;
      {
        const std::lock_guard<std::mutex> lock(held->mutex);
        stop = held->hold.Update(facade->DeviceOffline(), SteadyNowMs());
      }
      if (stop) {
        if (offline_stop) {
          offline_stop->store(true); // the result says "no connection" (the HTTP layer calls it a cancel)
        }
        return false;
      }
    }
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
      chat_.attachment_status = Tr(large ? "chat.attachment.uploading_stay" : "chat.attachment.uploading_percent",
                                   {{"percent", std::to_string(percent)}})
                                    .c_str();
      chat_.attachment_progress_width = (std::to_string(percent) + "%").c_str();
      DataModelHost::Instance().Dirty("chat", "attachment_status");
      DataModelHost::Instance().Dirty("chat", "attachment_progress_width");
      // The picture bubble: only its ring and its percent line change (every 5%), the row is not drawn again.
      if (const std::optional<std::string> id = pending_uploads_.Progress(percent);
          id && pending_uploads_.Queue().Find(*id)->ShowsIn(ActiveThreadId()) && context_ && context_->GetDocument(0)) {
        (void)PendingUploadRows::DrawProgress(context_->GetDocument(0), *id,
                                              pending_uploads_.Queue().Find(*id)->percent);
      }
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
  if (!failed_voice_path_.empty() && failed_voice_path_ != path) {
    std::filesystem::remove(failed_voice_path_, ec); // only one kept recording: the older one goes, not left behind
  }
  failed_voice_path_ = path;
  failed_voice_thread_ = thread_id;
  failed_record_id_.clear();
  if (thread_id == ActiveThreadId()) {
    ShowFailedAttachment(std::string(), std::filesystem::path(path).filename().string());
  }
}

void ChatController::OnDiscardAttachmentSend(const std::string& item_id) {
  if (!item_id.empty()) {
    // A bubble's ✕ (its id): that failed item only; its staged copy goes, then the chip shows any other one.
    const PendingUpload* item = pending_uploads_.Queue().Find(item_id);
    if (!facade_ || !item || item->state != PendingUploadState::Failed || !item->ShowsIn(ActiveThreadId())) {
      return;
    }
    if (item->record_id.empty()) {
      return; // a bubble acts only on its own staged copy
    }
    facade_->DiscardOutgoingAttachment(item->record_id);
    pending_uploads_.Apply(item_id, OnUploadDiscardTapped);
    RefreshAttachmentInTranscript();
    SyncDisplayFromThread();
    DirtyChatTurns();
    RefreshPendingAttachmentChip(/*force=*/true);
    return;
  }
  // No id: the chip above the composer (a send left over from before a restart, or a voice recording).
  if (FailedVoiceTakesTheTap(!failed_voice_path_.empty(), failed_voice_thread_, ActiveThreadId(),
                             pending_uploads_.Queue().ShowsIn(ActiveThreadId()))) {
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

void ChatController::OnCancelAttachmentSend(const std::string& item_id) {
  // A bubble's disc carries its id (only the item in flight cancels); the upload card's ✕ none.
  if (!item_id.empty()) {
    const PendingUpload* item = pending_uploads_.Queue().Find(item_id);
    if (!item || item->state != PendingUploadState::Uploading) {
      return;
    }
    if (std::chrono::steady_clock::now() < bubble_tap_guard_until_) {
      // The tap that started or retried this item, landing again on the cancel disc drawn in the same place.
      return;
    }
  }
  if (!chat_.attachment_uploading || !send_cancel_ || send_cancel_->exchange(true)) {
    return;
  }
  // The card goes when the send stops (the upload at its next step; a direct push when it answers). The bubble's
  // disc only spins until then.
  if (!item_id.empty()) {
    pending_uploads_.Apply(item_id, OnUploadCancelTapped);
    if (pending_uploads_.Queue().Find(item_id)->ShowsIn(ActiveThreadId())) {
      SyncDisplayFromThread();
      DirtyChatTurns();
    }
  }
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

void ChatController::OnRetryAttachmentSend(const std::string& item_id) {
  // A bubble's retry carries its id (that failed item, its own staged copy); the chip above the composer none.
  const PendingUpload* item = item_id.empty() ? nullptr : pending_uploads_.Queue().Find(item_id);
  if (!item_id.empty() && (!item || item->state != PendingUploadState::Failed || !item->ShowsIn(ActiveThreadId()))) {
    return;
  }
  if (!item && FailedVoiceTakesTheTap(!failed_voice_path_.empty(), failed_voice_thread_, ActiveThreadId(),
                                      pending_uploads_.Queue().ShowsIn(ActiveThreadId()))) {
    if (facade_) {
      // Refused while another send runs: the recording stays behind Retry and the pill says why.
      const std::string path = failed_voice_path_;
      const std::string thread_id = failed_voice_thread_;
      StartAttachmentUpload(path, OutgoingPickSource::File, /*voice_file=*/true, thread_id);
    }
    return;
  }
  // A bubble acts only on its own staged copy; the chip on the one it shows.
  const std::string record_id = item ? item->record_id : failed_record_id_;
  if (!facade_ || record_id.empty()) {
    return;
  }
  if (chat_.attachment_uploading || chat_.attachment_preparing || chat_.voice_recording) {
    UserFeedback::Fail(Tr("chat.attach.previous_still_sending")); // a recording or another send first
    return;
  }
  const std::string thread_id = ActiveThreadId();
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
  // A failed picture's bubble uploads again from 0% (the staged ciphertext is what goes; the thumbnail stays).
  const bool picture = item != nullptr;
  if (picture) {
    bubble_tap_guard_until_ = std::chrono::steady_clock::now() + kBubbleTapGuard;
    pending_uploads_.Apply(item_id, OnUploadRetryTapped);
    RefreshAttachmentInTranscript();
    SyncDisplayFromThread();
    DirtyChatTurns();
  }
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();
  // Same dialog as a first send when the space is full: free the oldest file, then send the staged ciphertext again.
  auto offline_stop = std::make_shared<std::atomic<bool>>(false);
  BlobQuotaRecoveryFlow::RunUploadAsync<ThreadMessage>(
      [this, thread_id, record_id, cancel, offline_stop](std::function<void(Roe<ThreadMessage>)> done) {
        facade_->ResumeAttachmentAsync(thread_id, record_id, std::move(done), MakeSendProgress(offline_stop), cancel);
      },
      [this, cancel, thread_id, record_id, picture, item_id, offline_stop](Roe<ThreadMessage> sent) {
        chat_.attachment_uploading = false;
        ResetUploadCard(std::string());
        chat_.attachment_status = "";
        chat_.attachment_draft_name = "";
        failed_record_id_.clear();
        SyncComposerInputState();
        const bool stale_chip =
            !sent && sent.error().category == 0 && sent.error().code == OutgoingAttachmentStore::kWrongThread;
        if (picture) {
          // A resume never deletes the staged copy: while it is there the bubble stays Failed (also after a cancel of
          // the retry), with the same record; only a copy that is really gone removes it.
          bool staged = false;
          if (!sent && !stale_chip) {
            for (const auto& record : facade_->PendingOutgoingAttachments(thread_id)) {
              staged = staged || record.id == record_id;
            }
          }
          pending_uploads_.Apply(item_id, [sent = static_cast<bool>(sent), staged](const PendingUpload& now) {
            return OnUploadResumeFinished(now, sent, staged);
          });
          RefreshAttachmentInTranscript();
          if (!sent && thread_id == ActiveThreadId()) {
            SyncDisplayFromThread(); // sent: the display sync below brings the real row in this same task
            DirtyChatTurns();
          }
        }
        if (!sent && !stale_chip && !cancel->load()) {
          UserFeedback::Fail(UserFeedback::AttachmentFailureMessage(sent.error(), facade_ && facade_->DeviceOffline(),
                                                                    offline_stop->load()));
        } else if (sent && cancel->load()) {
          UserFeedback::Fail(Tr("chat.attachment.already_sent")); // the cancel came after it reached the contact
        }
        RefreshPendingAttachmentChip(/*force=*/true); // still staged: Retry again; gone or sent: nothing shown
        if (sent) {
          SyncDisplayFromThread();
          DirtyChatTurns();
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
                                           const bool voice_file, const std::string& to_thread,
                                           const std::optional<PendingUploadKind> bubble, const uint64_t pick_serial) {
  // A picture, a video or a file shows as a bubble in the transcript; a voice recording keeps its own bar.
  const bool with_bubble = bubble.has_value() && !voice_file;
  const bool picture = with_bubble && *bubble == PendingUploadKind::Picture;
  const bool video = with_bubble && *bubble == PendingUploadKind::Video;
  // Always the chat the caller names (a pick's armed chat, a recording's own): never the one open by the time this
  // runs, so nothing can land in a chat it was not chosen for.
  const std::string& thread_id = to_thread;
  const bool busy = chat_.attachment_uploading || chat_.attachment_preparing;
  if (!facade_ || thread_id.empty() || !picks_.Accept(pick_serial, busy)) {
    EndPick(pick_serial);
    if (voice_file && !thread_id.empty()) {
      // A finished recording is never thrown away: it waits behind Retry, and the pill says why.
      KeepVoiceForRetry(path, thread_id);
      UserFeedback::Fail(Tr("chat.voice.upload_busy"));
      return;
    }
    release_pick_(path);
    if (busy) {
      // The entries are greyed meanwhile, so this is the safety net: never drop a pick without a word.
      UserFeedback::Fail(Tr("chat.attach.previous_still_sending"));
    }
    RefreshPickOverlay();
    return;
  }
  if (voice_file) {
    failed_voice_path_.clear(); // this attempt is the retry
    failed_voice_thread_.clear();
  }
  // A failed picture still shown in its chat gives way to this send (its staged copy stays, as the chip's did).
  for (const PendingUpload& item : std::vector<PendingUpload>(pending_uploads_.Queue().Items())) {
    if (pending_uploads_.Apply(item.row_id, OnOtherAttachmentStarted) && item.ShowsIn(ActiveThreadId()) &&
        messaging_ready_) {
      SyncDisplayFromThread();
      DirtyChatTurns();
    }
  }
  // What is staged in this chat already: a bubble's failure keeps the new record's id (its Retry / Discard act on that
  // copy, never on an older one the chip shows later).
  std::vector<std::string> staged_ids_before;
  if (voice_file || with_bubble) {
    for (const auto& record : facade_->PendingOutgoingAttachments(thread_id)) {
      staged_ids_before.push_back(record.id);
    }
  }
  const size_t staged_before = staged_ids_before.size();

  chat_.attachment_uploading = true;
  chat_.attachment_failed = false;
  attachment_thread_ = thread_id;
  failed_record_id_.clear();
  send_cancel_ = std::make_shared<std::atomic<bool>>(false);
  const auto cancel = send_cancel_;
  const std::string shown_name = DisplayNameFromPickedPath(path); // a content:// URI is not a name
  chat_.attachment_draft_name =
      (shown_name.empty() ? std::filesystem::path(path).filename().string() : shown_name).c_str();
  ResetUploadCard(chat_.attachment_draft_name.c_str());
  chat_.status = Tr("chat.attachment.uploading").c_str();
  chat_.attachment_status = Tr("chat.attachment.uploading").c_str();
  if (picture) {
    // The bubble at once, sized from the file's head (only the head is read here); a head that cannot be read here
    // (an Android content:// pick) gives 4:3 until the worker has the real size.
    std::optional<PicturePixels> upright;
    if (const auto header = ReadImageHeader(path)) {
      upright = UprightPixels(header->width, header->height, header->exif_orientation);
    }
    bubble_tap_guard_until_ = std::chrono::steady_clock::now() + kBubbleTapGuard;
    ApplyPendingUpload({StartPendingUpload(++pending_upload_serial_, thread_id, upright), {}});
    scroller_.RequestScrollToLatest(); // as a text send does
  } else if (with_bubble && *bubble == PendingUploadKind::File) {
    // The file's bubble at once: its name now, its size once the preparation has read it.
    PendingUpload file = StartPendingUpload(++pending_upload_serial_, thread_id, std::nullopt, PendingUploadKind::File);
    file.title = chat_.attachment_draft_name.c_str();
    bubble_tap_guard_until_ = std::chrono::steady_clock::now() + kBubbleTapGuard;
    ApplyPendingUpload({std::move(file), {}});
    scroller_.RequestScrollToLatest();
  } else if (video) {
    // A video's bubble waits for its poster (so its size never changes); no card meanwhile, the overlay instead.
    video_bubble_thread_ = thread_id;
    RefreshAttachmentInTranscript();
    ShowPick(pick_serial);
  }
  if (!video) {
    EndPick(pick_serial); // the file's bubble is up (a picture's choice ended its pick already)
  }
  SyncComposerInputState();
  DirtyChatChrome();
  DirtyChatHeader();

  const auto finish_uploading = [this, pick_serial, cancel]() {
    if (send_cancel_ != cancel) {
      return; // the overlay's Cancel let this send go already (another may run now): its state is not ours
    }
    chat_.attachment_uploading = false;
    video_bubble_thread_.clear();
    EndPick(pick_serial); // a video that failed or was cancelled before its bubble: the overlay goes
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
  const std::string row_id = picture ? pending_uploads_.Upload().row_id : std::string();
  const std::string shown_title = chat_.attachment_draft_name.c_str();
  AppRuntime::PostWorkerNormal([this, thread_id, path, source, limits, scratch_dir, finish_uploading, voice_file,
                                staged_before, staged_ids_before, cancel, picture, video, with_bubble, row_id,
                                shown_title, pick_serial, session = messaging_session_]() {
    if (video) {
      // The poster first, then the bubble, once: its size is known before it is drawn.
      const UploadThumbnail poster = WriteVideoPosterThumbnail(path, scratch_dir);
      AppRuntime::PostUI([this, thread_id, cancel, poster, shown_title, pick_serial, session]() {
        // Still the pick that started it (not ended by Cancel, the lock, a sign-out or the chat deleted), in the same
        // messaging session, and this send not cancelled or replaced: only then is the bubble built.
        const bool pick_current = picks_.IsCurrent(pick_serial);
        EndPick(pick_serial); // the bubble (or nothing) replaces the overlay
        const auto remove_poster = [&poster]() {
          if (!poster.path.empty()) {
            std::error_code ec;
            std::filesystem::remove(poster.path, ec);
          }
        };
        if (!pick_current || session != messaging_session_ || send_cancel_ != cancel || cancel->load()) {
          remove_poster(); // no bubble, and the plaintext poster goes
          if (send_cancel_ == cancel) {
            cancel->store(true); // the pick is over: its video is not sent either
            video_bubble_thread_.clear();
            RefreshAttachmentInTranscript();
          }
          return;
        }
        video_bubble_thread_.clear();
        std::optional<PicturePixels> size;
        if (poster.width > 0 && poster.height > 0) {
          size = PicturePixels{poster.width, poster.height};
        }
        PendingUpload item = StartPendingUpload(++pending_upload_serial_, thread_id, size, PendingUploadKind::Video);
        item.title = shown_title;
        if (messaging_ready_ && !MessagingReady()) {
          remove_poster(); // locked meanwhile: the plaintext goes, the frame keeps its size
        } else {
          item.thumb_path = poster.path;
        }
        bubble_tap_guard_until_ = std::chrono::steady_clock::now() + kBubbleTapGuard;
        ApplyPendingUpload({std::move(item), {}});
        if (thread_id == ActiveThreadId()) {
          scroller_.RequestScrollToLatest();
        }
      });
    }
    if (picture) {
      // The bubble's thumbnail first, before the picture is prepared, so it replaces the grey placeholder early.
      const UploadThumbnail bubble = WritePickedPictureThumbnail(path, scratch_dir, limits.max_original_image_bytes);
      AppRuntime::PostUI([this, row_id, bubble]() {
        if (pending_uploads_.Upload().row_id != row_id) {
          if (!bubble.path.empty()) {
            std::error_code ec;
            std::filesystem::remove(bubble.path, ec); // its row is gone (another send since)
          }
          return;
        }
        if (bubble.width > 0 && bubble.height > 0) {
          ApplyPendingUpload(OnUploadSizeRead(pending_uploads_.Upload(), {bubble.width, bubble.height}));
        }
        if (!bubble.path.empty()) {
          // The app locked meanwhile: the plaintext goes at once and the placeholder stays.
          const bool locked = messaging_ready_ && !MessagingReady();
          ApplyPendingUpload(OnUploadThumbnail(pending_uploads_.Upload(), bubble.path, locked));
        }
      });
    }
    auto prepared = PrepareOutgoingAttachment(path, limits, source, scratch_dir);
    // The card's small picture: not for a picture, whose bubble is in the transcript.
    std::string thumb = prepared && !voice_file && !with_bubble
                            ? WriteUploadThumbnail(path, *prepared, scratch_dir).path
                            : std::string();
    release_pick_(path); // read by now (and kept as a CAS copy once sent)
    AppRuntime::PostUI([this, thread_id, path, finish_uploading, voice_file, staged_before, staged_ids_before, cancel,
                        thumb, with_bubble, picture, prepared = std::move(prepared)]() mutable {
      if (send_cancel_ != cancel) {
        // Let go by the overlay's Cancel (another send may run now): nothing of this one goes on.
        if (!thumb.empty()) {
          std::error_code ec;
          std::filesystem::remove(thumb, ec);
        }
        return;
      }
      if (chat_.attachment_uploading && send_cancel_ == cancel && prepared) {
        chat_.attachment_thumb = thumb.c_str(); // the card shows it; ResetUploadCard deletes it
        chat_.attachment_type_label = AttachmentTypeLabel(prepared->filename, prepared->mime).c_str();
        DataModelHost::Instance().Dirty("chat", "attachment_thumb");
        DataModelHost::Instance().Dirty("chat", "attachment_type_label");
      } else if (!thumb.empty()) {
        std::error_code ec;
        std::filesystem::remove(thumb, ec);
      }
      if (with_bubble && prepared && !picture && send_cancel_ == cancel) {
        // A file / video bubble: the name it goes under and its size, now known.
        ApplyPendingUpload(OnUploadDetails(pending_uploads_.Upload(), prepared->filename,
                                           FormatAttachmentByteSize(prepared->bytes.size())));
      }
      if (!prepared) {
        finish_uploading();
        if (with_bubble) {
          // Nothing was staged: the bubble goes; the pill says why unless the person had cancelled.
          ApplyPendingUpload(OnUploadSendFinished(pending_uploads_.Upload(), UploadSendOutcome::FailedNotStaged));
        }
        if (!(with_bubble && cancel->load())) {
          UserFeedback::Fail(UserFeedback::UserMessage(prepared.error()));
        }
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
      auto offline_stop = std::make_shared<std::atomic<bool>>(false); // set when the progress stopped it for offline
      BlobQuotaRecoveryFlow::RunUploadAsync<ThreadMessage>(
          [this, thread_id, attachment, attempts, cancel, staged_ids_before,
           offline_stop](std::function<void(Roe<ThreadMessage>)> done) {
            if ((*attempts)++ == 0) {
              facade_->SendAttachmentAsync(thread_id, std::move(*attachment), std::move(done),
                                           MakeSendProgress(offline_stop), cancel);
              return;
            }
            // This send's own staged copy (not one left by an earlier send in this chat).
            std::string own;
            for (const auto& record : facade_->PendingOutgoingAttachments(thread_id)) {
              if (std::find(staged_ids_before.begin(), staged_ids_before.end(), record.id) == staged_ids_before.end()) {
                own = record.id;
              }
            }
            if (own.empty()) {
              done(Error("The attachment is no longer staged").WithUser(Tr("chat.attachment.pick_again")));
              return;
            }
            facade_->ResumeAttachmentAsync(thread_id, own, std::move(done), MakeSendProgress(offline_stop), cancel);
          },
          [this, thread_id, path, finish_uploading, voice_file, staged_before, staged_ids_before, cancel, with_bubble,
           offline_stop](Roe<ThreadMessage> sent) {
            if (send_cancel_ != cancel && !voice_file) {
              return; // let go by the overlay's Cancel: another send may own the screen now
            }
            finish_uploading();
            const bool cancelled = !sent && cancel->load(); // nothing was sent and nothing is kept: no message
            if (with_bubble) {
              // Sent: the bubble goes here and the real row comes with the display sync below, in this same task.
              std::string new_record;
              if (!sent && !cancelled) {
                for (const auto& record : facade_->PendingOutgoingAttachments(thread_id)) {
                  if (std::find(staged_ids_before.begin(), staged_ids_before.end(), record.id) ==
                      staged_ids_before.end()) {
                    new_record = record.id;
                  }
                }
              }
              const UploadSendOutcome outcome = sent                  ? UploadSendOutcome::Sent
                                                : cancelled           ? UploadSendOutcome::Cancelled
                                                : !new_record.empty() ? UploadSendOutcome::FailedStaged
                                                                      : UploadSendOutcome::FailedNotStaged;
              PendingUploadStep step = OnUploadSendFinished(pending_uploads_.Upload(), outcome);
              step.next.record_id = new_record;
              ApplyPendingUpload(std::move(step),
                                 /*redraw=*/!sent);
            }
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
                UserFeedback::Fail(UserFeedback::AttachmentFailureMessage(
                    sent.error(), facade_ && facade_->DeviceOffline(), offline_stop->load()));
              }
              RefreshPendingAttachmentChip(/*force=*/true); // a kept ciphertext shows Retry
              return;
            }
            if (cancel->load()) {
              UserFeedback::Fail(Tr("chat.attachment.already_sent")); // the cancel came after it reached the contact
            }
            SyncDisplayFromThread();
            if (with_bubble) {
              DirtyChatTurns();
            }
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
                                                                std::optional<int64_t> newest_inclusive) {
  if (!facade_) {
    return {};
  }
  TranscriptFacts facts = facade_->TranscriptFactsFor();
  facts.voice_supported = VoiceMessagesSupported();
  facts.thread_is_group = chat_.thread_is_group;
  facts.voice_max_seconds = facade_->CurrentAttachmentLimits().voice_max_seconds;
  facts.voice_state = [this](const std::string& message_id) { return voice_playback_->State(message_id); };
  facts.audio_files_play = voice_playback_->PlaysFiles();
  const int64_t now_ms = util::NowUnixMs();
  std::optional<int64_t> next_mark_change;
  std::vector<std::string> red_ids;
  const std::optional<std::vector<int64_t>> group_others = presenter_.GroupOthersJoinedAt(thread_id);
  facts.send_failure_line = [this, thread_id](const ThreadMessage& failure_row) -> std::optional<std::string> {
    // "The server can't find them" says how a tap on the red mark goes on (update their card, and resend or not).
    const auto reason = SendFailureRowReason(failure_row);
    const auto message_id = SendFailureRowMessageId(failure_row);
    const auto failure = reason ? SendFailureFromWire(*reason) : std::nullopt;
    if (!message_id || (failure != SendFailure::RecipientNotFound && failure != SendFailure::SenderNotRegistered)) {
      return std::nullopt;
    }
    return SendFailureText(*failure, presenter_.SendAdviceOf(thread_id, *message_id));
  };
  facts.send_mark = [this, now_ms, &next_mark_change, &group_others, &red_ids](const ThreadMessage& message) {
    const SendMark mark = presenter_.SendMarkOf(message, now_ms, next_mark_change, group_others);
    if (mark == SendMark::Failed) {
      red_ids.push_back(message.id);
    }
    return mark;
  };
  // The picture being sent is drawn after the newest message as our own (kept aside, put back by the display sync).
  std::vector<MessageDisplayRow> rows =
      pending_uploads_.Render(facade_->BuildTranscript(thread_id, oldest_inclusive, newest_inclusive), facts,
                              presenter_.MentionRosterOf(thread_id), thread_id, !newest_inclusive.has_value());
  agent_.DecorateAiImageRows(rows, thread_id);
  ArmSendMarkRedraw(next_mark_change);
  TellResentTurnedRed(thread_id, red_ids);
  return rows;
}

void ChatController::TellResentTurnedRed(const std::string& thread_id, const std::vector<std::string>& red_ids) {
  // Hooked to the drawing that turns a clock red (a final failure, or ArmSendMarkRedraw at the clock's limit), so a
  // message sent again by a tap and red again says why once — in its own chat, while it is looked at; else it waits.
  const std::optional<std::string> resent = presenter_.ResentTurnedRed(thread_id, red_ids);
  if (!resent || !ShowsResentFailurePill(thread_id, ActiveThreadId(), AppLifecycle::IsUserAttentive())) {
    return;
  }
  presenter_.ForgetResent(*resent);
  ShowToast(SendFailureReasonText(presenter_.SendFailureReasonOf(thread_id, *resent), SendMarkTap::Refusal::None,
                                  presenter_.SendAdviceOf(thread_id, *resent)));
}

void ChatController::ArmSendMarkRedraw(const std::optional<int64_t> at_ms) {
  if (!at_ms || (send_mark_timer_ != 0 && send_mark_redraw_at_ <= *at_ms)) {
    return; // nothing to wait for, or a redraw at least as soon is armed
  }
  if (send_mark_timer_ != 0) {
    AppRuntime::CancelCoordinatorTimer(send_mark_timer_);
  }
  const uint64_t turn = ++send_mark_redraw_turn_;
  send_mark_redraw_at_ = *at_ms;
  const auto delay = std::chrono::milliseconds(std::max<int64_t>(0, *at_ms - util::NowUnixMs()));
  send_mark_timer_ =
      AppRuntime::ScheduleCoordinatorOneShot(delay, [this, turn, alive = std::weak_ptr<int>(send_mark_alive_)]() {
        AppRuntime::PostUI([this, turn, alive]() {
          if (!alive.lock() || turn != send_mark_redraw_turn_) {
            return;
          }
          send_mark_timer_ = 0;
          send_mark_redraw_at_ = 0;
          RedrawSendMarks();
        });
      });
}

void ChatController::RedrawSendMarks() {
  if (!messaging_ready_) {
    return;
  }
  SyncDisplayFromThread();
  DirtyChat();
  NotifySurfaceChanged();
}

void ChatController::OnSendMarkTapped(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty()) {
    return;
  }
  const std::string thread_id = ActiveThreadId();
  ApplySendMarkTap(thread_id, message_id, presenter_.TapSendMark(thread_id, message_id));
}

void ChatController::ApplySendMarkTap(const std::string& thread_id, const std::string& message_id,
                                      const SendMarkTap& tap) {
  switch (tap.kind) {
    case SendMarkTap::Kind::Nothing:
      return;
    case SendMarkTap::Kind::UpdateTheirCard: {
      // The server could not find them: update their card first, and send this again when it is resendable.
      const SendFixDialog dialog = UpdateTheirCardDialog(tap.resendable);
      UserFeedback::Confirm(
          dialog.title, dialog.body,
          [this, thread_id, message_id, contact_id = tap.contact_id, resendable = tap.resendable,
           session = messaging_session_](const bool ok) {
            // The answer of a dialog from before a lock / unlock belongs to no session now.
            if (ok && messaging_ready_ && session == messaging_session_) {
              send_fix_.UpdateTheirCard(thread_id, message_id, contact_id, resendable);
            }
          },
          dialog.ok_label);
      return;
    }
    case SendMarkTap::Kind::UpdateMyCard: {
      const SendFixDialog dialog = UpdateMyCardDialog();
      UserFeedback::Confirm(
          dialog.title, dialog.body,
          [this, thread_id, message_id, resendable = tap.resendable, session = messaging_session_](const bool ok) {
            if (ok && messaging_ready_ && session == messaging_session_) {
              // B216's action: it waits for an automatic renewal in flight (RunWhenRegistrationIdle) itself.
              send_fix_.UpdateMyCard(thread_id, message_id, resendable);
            }
          },
          dialog.ok_label);
      return;
    }
    case SendMarkTap::Kind::StillTrying:
      ShowToast(Tr("chat.send.still_trying"));
      return;
    case SendMarkTap::Kind::ReasonOnly:
      ShowToast(SendFailureReasonText(presenter_.SendFailureReasonOf(thread_id, message_id), tap.refusal));
      return;
    case SendMarkTap::Kind::RemovalFailed:
      // Its new copy already went out on an earlier tap; nothing is sent a third time.
      ShowToast(Tr("chat.send.not_sent"));
      return;
    case SendMarkTap::Kind::SendFailed:
      // As the composer says it; the old bubble stays as it was.
      UserFeedback::Fail(PaymentErrorUserMessage(tap.error.value_or(Error("Send failed"))));
      return;
    case SendMarkTap::Kind::Resent:
      // The new message is at the bottom (the draft untouched); the old row is gone unless it was delivered meanwhile.
      if (thread_id == ActiveThreadId()) {
        scroller_.RequestScrollToLatest(); // only the chat that is open (a resend after a card update may be another)
      }
      RefreshFromMessaging();
      return;
  }
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
                                       facts.sender_icon ? facts.sender_icon(sender) : std::string(),
                                       /*from_dm=*/false);
  }
  // The group's owner finds the "Group admin" card at the bottom, as from the member list.
  if (people_picker_notify_.offer_group_admin) {
    people_picker_notify_.offer_group_admin(facade_->ActiveThreadId(), sender);
  }
}

void ChatController::OpenPicture(const std::string& message_id) {
  if (!messaging_ready_ || !facade_ || message_id.empty() || opening_message_ids_.count(message_id) != 0) {
    return;
  }
  // A spinner in this frame (B203: no picture under it, the bubble only has the small preview); the system viewer
  // opens over it with the full picture, which OpenAttachment finds from the message.
  if (shell_feedback_.mount_photo_preview && photo_preview_.Begin(message_id)) {
    shell_feedback_.mount_photo_preview(PhotoPreviewRml());
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
      log().info
          << "Attachment tap: lookup " << lookup_ms << " ms, worker " << worker_ms << " ms, to open call "
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

size_t ChatController::FirstScreenMessages(const ThreadKind kind) {
  return kind == ThreadKind::Group ? kChatFirstScreenMessagesGroup : kChatFirstScreenMessagesDirect;
}

void ChatController::SyncDisplayFromThread() {
  if (!messaging_ready_) {
    return;
  }
  Roe<Thread> active = facade_ ? facade_->GetActiveThread() : Roe<Thread>::error(Error("no chat port"));
  if (!active) {
    CloseSearch(); // no chat open (Home, or the chat was closed)
    chrome_.ResetPanelState();
    agent_.RefreshBusyForActiveThread();
    SyncDraftOwner();
    return;
  }
  const auto sync_started = std::chrono::steady_clock::now();
  const std::string thread_id = ActiveThreadId();
  const bool thread_changed = scroller_.BeginDisplaySync(thread_id, FirstScreenMessages(active->kind));
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
    backfill_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  }

  // The real rows only: the picture being sent at the end is not a message (the counts and the tail stay as without it).
  const auto [prev_tail_id, prev_count] = PendingUploadRows::RealTail(chat_.messages);

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
  // Last, after everything that counts rows or reads the tail: the picture being sent, at the end of its chat.
  if (pending_uploads_.KeepAtTail(chat_.messages, thread_id, !scroller_.LoadedMaxDisplayOrder())) {
    chat_.has_turns = true;
  }
  agent_.RefreshBusyForActiveThread(); // "thinking", the stop key and a queued bubble follow the open thread
  SyncDraftOwner();                    // so does the unsent text
  EndEditIfTargetGone();               // the message being edited was deleted on another device

  // Runs on the UI thread for every message, reaction and AI reply: report the slow ones.
  const auto sync_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sync_started).count();
  if (open_probe_.armed) {
    open_probe_.armed = false;
    open_probe_.layout_pending = true;
    open_probe_.rows = static_cast<int64_t>(chat_.messages.size());
    open_probe_.pictures = static_cast<int64_t>(CountPictureBubbleRows(chat_.messages));
    open_probe_.placeholders = static_cast<int64_t>(CountPicturePlaceholderRows(chat_.messages));
    open_probe_.build_ms = sync_ms; // the same stretch chat.sync_slow measures
    open_probe_.first_screen = scroller_.FirstScreenResult();
    open_probe_.built_at = std::chrono::steady_clock::now();
    // chat.previews: from the open (the sync started then) until the placeholders drawn now have their previews.
    previews_probe_.Start(PicturePlaceholderHashes(chat_.messages), sync_started, UiFrameStats::SlowFrames());
  }
  SyncPictureTextures(); // rows gone (deleted, another chat, trimmed): their pictures' textures go
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
  ShowConfirmWithCheckbox(
      Tr("chat.open_link_title"), Tr("chat.open_link_from", {{"host", UrlHost(url)}}), Tr("chat.open_link_skip"), false,
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
      ShowConfirm(Tr("chat.fork_group.title"), Tr("chat.fork_group.body"), [this, message, confirmed_payload](bool ok) {
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
  if (presenter_.EditingIn(CurrentDraftKey())) {
    SendEditedMessage();
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
    UserFeedback::Fail(PaymentErrorUserMessage(sent.error()));
    RestoreDraftIfEmpty(thread_id, reply); // its mentions stay recorded, so sending again marks them
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
        .when =
            [this](ui::Context* ctx) {
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
    return; // already first: nothing to write
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

bool ChatController::IsCallRecord(const std::string& message_id) const {
  if (!facade_ || message_id.empty()) {
    return false;
  }
  auto found = facade_->GetMessage(ActiveThreadId(), message_id);
  return found && found->has_value() && CallSummaryRowOf(**found).has_value();
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
  if (chat_.compose_disabled || text.empty() || chat_.editing_message) {
    return; // editing a message is not replying: the edit's words stay as typed
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
  auto* draft =
      ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById("draft-input"));
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

bool ChatController::CanEditMessage(const std::string& message_id) {
  return messaging_ready_ && presenter_.CanEditMessage(message_id);
}

void ChatController::StartEditMessage(const std::string& message_id) {
  const std::string thread_id = CurrentDraftKey(); // the chat on screen: its composer is the one shown
  if (chat_.compose_disabled || thread_id.empty() || thread_id != ActiveThreadId()) {
    return;
  }
  if (presenter_.EditingIn(thread_id)) {
    return; // one edit at a time: the words being edited stay as typed (the menu offers no "Edit" meanwhile)
  }
  const auto text = presenter_.BeginEdit(thread_id, message_id, chat_.draft.c_str());
  if (!text) {
    ShowToast(Tr("chat.edit.too_late"));
    return;
  }
  CancelQuoteReply(); // editing and replying do not mix
  if (chat_.voice_mode) {
    ToggleVoiceMode(); // back to the keyboard: the words are edited there
  }
  chat_.draft = text->c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  SyncEditingState();
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  auto* draft =
      ui_dynamic_cast<ui::ElementFormControlTextArea*>(context_->GetDocument(0)->GetElementById("draft-input"));
  if (!draft) {
    return;
  }
  draft->SetValue(text->c_str());
  draft->Focus(); // the keyboard comes up, the caret at the end
  const ui::String current = draft->GetValue();
  const int end = ui::StringUtilities::ConvertByteOffsetToCharacterOffset(current, static_cast<int>(current.size()));
  draft->SetSelectionRange(end, end);
}

void ChatController::CancelEditMessage() {
  const std::string thread_id = CurrentDraftKey();
  if (!presenter_.EditingIn(thread_id)) {
    return;
  }
  chat_.draft = presenter_.CancelEdit(thread_id).c_str(); // the draft from before; the edited words go (their choice)
  DataModelHost::Instance().Dirty("chat", "draft");
  SyncEditingState();
}

void ChatController::SendEditedMessage() {
  const std::string thread_id = CurrentDraftKey();
  const std::string typed = util::Trim(chat_.draft.c_str());
  if (typed.empty()) {
    return; // an empty edit is not sent (deleting is how a message goes away)
  }
  if (auto valid = ChatPresenter::ValidateDraft(typed); !valid) {
    ShowToast(Tr(valid.error().message));
    return;
  }
  // The edit's send time is now, the tap (DEV/PLAN-message-edit.md): its receivers judge the 15 minutes by it.
  const ChatPresenter::EditSendResult result = presenter_.SendEdit(thread_id, chat_.draft.c_str(), util::NowUnixMs());
  using Kind = ChatPresenter::EditSendResult::Kind;
  switch (result.kind) {
    case Kind::None:
      return;
    case Kind::TooLate:
      ShowToast(Tr("chat.edit.too_late")); // still editing: the words stay in the input
      return;
    case Kind::TooLong:
      ShowToast(Tr("chat.error.message_too_long")); // as a message too long to send; still editing, words kept
      return;
    case Kind::Failed:
      UserFeedback::Fail(PaymentErrorUserMessage(result.error.value_or(Error("Send failed"))));
      return;
    case Kind::TargetGone:
      ShowToast(Tr("chat.edit.target_deleted"));
      break;
    case Kind::Unchanged:
    case Kind::Sent:
      break;
  }
  chat_.draft = result.composer.c_str();
  DataModelHost::Instance().Dirty("chat", "draft");
  SyncEditingState();
  SyncDisplayFromThread(); // the bubble shows the new words and "Edited"
  DirtyChatTurns();
  NotifySurfaceChanged();
}

void ChatController::EndEditIfTargetGone() {
  const std::string thread_id = CurrentDraftKey();
  if (!thread_id.empty() && presenter_.EditingIn(thread_id)) {
    if (const auto composer = presenter_.EndEditIfTargetGone(thread_id, chat_.draft.c_str())) {
      ShowToast(Tr("chat.edit.target_deleted"));
      chat_.draft = composer->c_str();
      DataModelHost::Instance().Dirty("chat", "draft");
    }
  }
  // Also when the chat shown changed without a draft hand-over (a new chat): the bar follows the chat shown.
  if (chat_.editing_message != (presenter_.EditingIn(CurrentDraftKey()) != nullptr)) {
    SyncEditingState();
  }
}

void ChatController::SyncEditingState() {
  const bool editing = presenter_.EditingIn(CurrentDraftKey()) != nullptr; // Home's composer never edits
  if (chat_.editing_message != editing) {
    chat_.editing_message = editing;
    DataModelHost::Instance().Dirty("chat", "editing_message");
    // The reply bar's line says "Edit message" meanwhile (a reply and an edit never show together).
    if (editing || !chat_.quote_reply) {
      chat_.quote_reply_text = editing ? StructuredTextParser::EscapeText(Tr("chat.edit.bar")).c_str() : "";
      DataModelHost::Instance().Dirty("chat", "quote_reply_text");
    }
  }
  SyncComposerInputState(); // the plus and the microphone grey out while editing
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
  if (util::Trim(chat_.draft.c_str()).empty()) { // never overwrite what the user has typed
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
  SyncHomeFavoriteSize(); // a turn or a resize changes the width the row divides
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
  if (SendStartsHomeConversation(ChromeSnapshot())) {
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
    expect_agent_work = facade_->ExpectsAgentWork(ActiveThreadId(), trimmed, user_payload);
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
      agent_.HoldSend({.text = trimmed,
                       .payload = std::move(user_payload),
                       .from_home_chip = from_home_chip,
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
      pending_reply_ = PendingReply{.entry_id = std::string(),
                                    .thread_id = thread_id,
                                    .output = MockAssistantRespond(trimmed),
                                    .from_llm = false};
      return;
    }
    log().debug << "Using mock assistant response";
    agent_.MarkAgentBusy(thread_id);
    pending_reply_ = PendingReply{.entry_id = user_message.id,
                                  .thread_id = thread_id,
                                  .output = MockAssistantRespond(trimmed),
                                  .from_llm = false};
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
            UserFeedback::Fail(PaymentErrorUserMessage(routed.error()));
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
      UserFeedback::Fail(PaymentErrorUserMessage(routed.error()));
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
  SyncEditingState(); // an edit waits in its own chat (its words came back with the draft)
}

void ChatController::ForgetComposerExtras(const std::string& key) {
  drafts_.Forget(key);
  quotes_.erase(key);
  presenter_.ForgetEdit(key);
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
      UserFeedback::Fail(PaymentErrorUserMessage(routed.error()));
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
    mentions_.Close(); // the `@` picker first; the draft keeps its `@`
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
  unlock_ensure_.ensure_unlocked([this, action = std::move(action)](const bool unlocked) {
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
  // A new session: results of tasks the last one sent off are dropped when they come back. A run still out keeps its
  // slot (one at a time); what is triggered below runs once more after it.
  ++messaging_session_;
  // Pre-send image previews are session files; a crash or a kill leaves them behind, so start clean.
  if (const std::string profile_dir = facade_ ? facade_->ProfileDataDir() : std::string(); !profile_dir.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(AttachmentViewRoot(profile_dir, kAiImageViewThread), ec);
  }
  {
    StartupPhase phase("ChatController::WireMessagingBindings/EnsureBriefGuestLlmKey");
    agent_.EnsureBriefGuestLlmKey();
  }
  {
    StartupPhase phase("ChatController::WireMessagingBindings/RefreshLlmSetupBanner");
    agent_.RefreshLlmSetupBanner();
  }
  if (IThreadStore* store = facade_ ? facade_->ThreadStore() : nullptr) {
    if (agent_.Ports().set_thread_store) {
      agent_.Ports().set_thread_store(store);
    }
  }
  chat_.compose_disabled = false;
  DirtyChatChrome();
  facade_->SetOnMessagesChanged([this]() {
    presenter_.ForgetDeliveredResends(); // a message sent again by a tap and delivered: no failure pill to keep for it
    RefreshFromMessaging();
    if (contacts_notify_.refresh) {
      contacts_notify_.refresh();
    }
  });
  facade_->SetOnDeliveryNotice([this](const DeliveryNotice& notice) {
    // A send that did not go out is told in its chat, as a line under the message; a toast only when that has no
    // place (no chat, or the cause is unknown). An unreadable inbound message has its line already.
    switch (presenter_.ApplyDeliveryNotice(notice)) {
      case NoticeTell::Nothing:
        break;
      case NoticeTell::Toast:
        ShowToast(DeliveryNoticeText(notice));
        break;
    }
    RefreshFromMessaging();
    NotifySurfaceChanged();
  });
  facade_->SetOnBackgroundUnread([this](std::string title, std::string body, std::string thread_id, bool mentions_us) {
    const ProfilePreferences& profile_prefs = Store().Snapshot().profile_prefs;
    if (!profile_prefs.show_notifications) {
      return;
    }
    body =
        NotificationBodyFor(profile_prefs.show_notification_preview, std::move(body), Tr("notification.hidden_body"));
    // A muted thread still counts as unread in the list; it just does not notify — unless the message
    // mentions us (G015), and then once a minute per group at most.
    const std::optional<ThreadLocalPrefs> prefs = presenter_.PrefsOf(thread_id);
    const int64_t now_ms = util::NowUnixMs();
    const bool muted = prefs && ThreadIsMuted(prefs->muted_until, now_ms);
    if (!NoticeAlerts(muted, prefs && prefs->mute_mentions, mentions_us, muted_mention_alert_ms_[thread_id], now_ms)) {
      return;
    }
    // A stranger's first message arrives before the directory answered, so the title would be a bare
    // id. Give the lookup a moment and use whatever name is known by then.
    bool bare_id = false;
    if (auto thread = facade_->GetThread(thread_id); thread && *thread) {
      bare_id = facade_->ResolveThreadLabel(**thread).trust == PeerLabelTrust::RawId;
    }
    // The unread count now (the message that raised this notice is in it): Android's notification number.
    const auto unread_of = [this](const std::string& id) {
      auto thread = facade_->GetThread(id);
      return thread && *thread ? (*thread)->unread_count : 0;
    };
    if (!bare_id) {
      ILocalNotifier::Instance().NotifyIncoming(title, body, thread_id, unread_of(thread_id));
      return;
    }
    AppRuntime::PostWorkerAndReplyOnUI<int>(
        WorkerLane::Normal,
        []() {
          std::this_thread::sleep_for(std::chrono::milliseconds(1500));
          return 0;
        },
        [this, body, thread_id, title, unread_of](int) {
          std::string resolved = title;
          if (auto thread = facade_->GetThread(thread_id); thread && *thread) {
            const std::string now = facade_->ResolveThreadLabel(**thread).title;
            if (!now.empty()) {
              resolved = now;
            }
          }
          ILocalNotifier::Instance().NotifyIncoming(resolved, body, thread_id, unread_of(thread_id));
        });
  });
  const auto activate = [this](std::string thread_id) {
    DesktopWindowChrome::RaiseAndFocus();
    if (!thread_id.empty()) {
      OnSelectThread(thread_id);
    }
  };
  ILocalNotifier::Instance().SetActivationHandler(activate);
  // A message or call alert's tap waits for the message (or the missed call's summary) it announced to sync,
  // then opens its chat.
  facade_->SetOnInboundMessage([this](std::string id, std::string thread_id) {
    if (PendingNotificationTap::Instance().TakeHintedRoute(id, PendingNotificationTap::Clock::now())) {
      OnSelectThread(thread_id);
    }
  });
  // A tap that arrived before unlock found no handler; it is still waiting.
  if (auto pending = PendingNotificationTap::Instance().TakeThread()) {
    activate(std::move(*pending));
  }
  // What the tap names may have synced before it, or before this wiring: look it up.
  {
    auto& tap = PendingNotificationTap::Instance();
    const auto now = PendingNotificationTap::Clock::now();
    if (const std::string hint = tap.FreshHint(now); !hint.empty()) {
      const auto thread_id = facade_->ThreadOfInbound(hint);
      if (thread_id) {
        DiagLine("notification.tap").Add("what", "lookup_found").Emit();
      } else {
        DiagLine("notification.tap").Add("what", "lookup_notfound").Emit();
      }
      if (thread_id && tap.TakeHintedRoute(hint, now)) {
        OnSelectThread(*thread_id);
      }
    }
  }
  // Relay poll is armed by ConversationsHub::StartCoordinatorTimers (not here). Call-wake UI
  // refresh is wired via ConversationsHub::SetOnCallWake from Application.
  // Push binding is HTTP: off the UI thread, one run at a time (a token change while one runs: once more after it).
  IPushDeviceRegistrar::SetTokenChangedHandler([this](const std::string& /*token*/) {
    AppRuntime::PostUI([this]() {
      if (!MessagingReady()) {
        return;
      }
      SyncPushDevices();
    });
  });
  {
    StartupPhase phase("ChatController::WireMessagingBindings/SyncPushDevices");
    SyncPushDevices();
  }
  facade_->SetOnThreadChanged([this]() {
    RefreshFromMessaging();
    if (contacts_notify_.refresh) {
      contacts_notify_.refresh();
    }
  });
  if (facade_ && facade_->HasRouter()) {
    facade_->SetOnLocalAction([this](const std::string& message, const std::optional<std::string>& payload) {
      HandleLocalAction(message, payload, {});
    });
    facade_->SetSharedAiConfirmCallback([this](const std::string& thread_id, const AtAiMode mode,
                                               const std::string& /*prompt*/,
                                               std::function<void(bool confirmed, bool dont_ask_again)> done) {
      ShowShareWithAiDialog(thread_id, mode, std::move(done));
    });
  }
  facade_->SetOnActionMessage([this](const ActionNotice& notice) {
    ShowToast(ActionNoticeText(notice));
    NotifySurfaceChanged();
  });
  RefreshFromMessaging();
  {
    StartupPhase phase("ChatController::WireMessagingBindings/TailSyncActiveE2eThread");
    facade_->TailSyncActiveE2eThread();
  }

  {
    // Off the UI thread; the result is stored back on it (MessagingReadyTasks), with the expiry hint.
    StartupPhase phase("ChatController::WireMessagingBindings/MaybeAutoRenewRegistration");
    if (ready_tasks_) {
      ready_tasks_->RenewRegistration();
    }
  }
  // Always reload so Brief key from identity is applied after unlock (not only on renew).
  {
    StartupPhase phase("ChatController::WireMessagingBindings/ReloadAgentConfig");
    agent_.ReloadAgentConfig();
  }
  {
    StartupPhase phase("ChatController::WireMessagingBindings/RefreshLlmSetupBanner");
    agent_.RefreshLlmSetupBanner();
  }
}

void ChatController::BindSharedEvents(ui::DataModelConstructor& ctor) {
  ctor.BindEventCallback("new_message", DataEvent([this] { OnNewMessage(); }));
  ctor.BindEventCallback("send_chat_action",
                         DataEvent<std::string, int>([this](const std::string& entry_id, int index) {
                           if (index >= 0) {
                             agent_.SendChatAction(entry_id, index);
                           }
                         }));
  ctor.BindEventCallback("submit_form", DataEvent<std::string, std::string>(
                                            [this](const std::string& form_id, const std::string& values_json) {
                                              agent_.SubmitForm(form_id, values_json);
                                            }));
  ctor.BindEventCallback(
      "calendar_prev", DataEvent<std::string>([this](const std::string& entry_id) { agent_.CalendarPrev(entry_id); }));
  ctor.BindEventCallback(
      "calendar_next", DataEvent<std::string>([this](const std::string& entry_id) { agent_.CalendarNext(entry_id); }));
  ctor.BindEventCallback("select_calendar_day", DataEvent<std::string, std::string>(
                                                    [this](const std::string& entry_id, const std::string& day) {
                                                      agent_.SelectCalendarDay(entry_id, day);
                                                    }));
  ctor.BindEventCallback("open_working_set",
                         DataEvent<std::string, int>([this](const std::string& entry_id, int block) {
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
  // Timers may not have run while the app was suspended: no clock may stay past its time (#210).
  lifecycle_foreground_id_ =
      AppLifecycle::AddForegroundListener([this, alive = std::weak_ptr<int>(send_mark_alive_)]() {
        AppRuntime::PostUI([this, alive]() {
          if (alive.lock()) {
            RedrawSendMarks();
          }
        });
      });
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
        .when =
            [](ui::Context* ctx) {
              ui::Element* focus = ctx ? ctx->GetFocusElement() : nullptr;
              return focus && focus->GetId() == "draft-input";
            },
        .action =
            [this]() {
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
        ctor.BindEventCallback(
            "send_suggestion", DataEvent<std::string>([this](const std::string& text) {
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
        ctor.BindEventCallback("send_suggestion_action",
                               DataEvent<std::string>([this](const std::string& id) { SendSuggestionAction(id); }));
        ctor.BindEventCallback("open_chat_link",
                               DataEvent<std::string, int>(
                                   [this](const std::string& entry_id, int link) { OpenChatLink(entry_id, link); }));
        ctor.BindEventCallback("stop_turn", DataEvent([this] { agent_.OnStopTurn(); }));
        ctor.BindEventCallback("cancel_queued", DataEvent([this] { CancelQueuedHere(); }));
        // The row of a picture being sent (pending-upload-…) is no message: none of the row events below acts on it.
        ctor.BindEventCallback("toggle_reaction", DataEvent<std::string, std::string>(
                                                      [this](const std::string& message_id, const std::string& emoji) {
                                                        if (IsPendingUploadRowId(message_id)) {
                                                          return;
                                                        }
                                                        ToggleReaction(message_id, emoji);
                                                      }));
        ctor.BindEventCallback("open_reactors", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OpenReactors(message_id);
                               }));
        ctor.BindEventCallback("reactor_remove", DataEvent<std::string>([this](const std::string& emoji) {
                                 const std::string message_id = reactors_message_id_;
                                 if (presenter_.MyReactionOn(message_id, emoji)) {
                                   ToggleReaction(message_id, emoji);
                                 }
                                 if (reactors_layer_ >= 0 && shell_navigation_.close_layer) {
                                   shell_navigation_.close_layer(reactors_layer_); // the coordinator clears the rows
                                 }
                               }));
        ctor.BindEventCallback("open_emoji_insert",
                               DataEventAt([this](ui::Event& ev) { menus_.OpenEmojiInsert(&ev); }));
        ctor.BindEventCallback("attach_file", DataEventAt([this](ui::Event& ev) { OnAttachFile(&ev); }));
        ctor.BindEventCallback(
            "home_attach_menu", DataEventAt([this](ui::Event& ev) {
              if (!messaging_ready_ || !facade_ || chat_.composer_input_disabled || chat_.attachment_uploading ||
                  chat_.attachment_preparing || !chat_.show_attach_button) {
                return;
              }
              if (!agent_.AgentCloudReady()) {
                UserFeedback::Note(Tr("ai.image.not_ready")); // the key stays put; the AI is just not connected yet
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
        ctor.BindEventCallback("play_voice", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 voice_playback_->Toggle(message_id);
                               }));
        ctor.BindEventCallback("play_audio_file", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 voice_playback_->ToggleFile(message_id);
                               }));
        ctor.BindEventCallback("audio_seek_press",
                               DataEventAt([this](ui::Event& ev) { voice_playback_->SeekPress(ev); }));
        ctor.BindEventCallback("show_send_failure", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OnSendMarkTapped(message_id);
                               }));
        // A bubble's events carry its item id; the chip above the composer sends none (an empty id).
        const auto with_item_id = [](auto handler) {
          return [handler](ui::DataModelHandle, ui::Event&, const ui::VariantList& args) {
            handler(EventArgAsString(args, 0).value_or(std::string()));
          };
        };
        ctor.BindEventCallback("cancel_picking", DataEvent([this] { OnCancelPicking(); }));
        ctor.BindEventCallback("retry_attachment_send",
                               with_item_id([this](const std::string& id) { OnRetryAttachmentSend(id); }));
        ctor.BindEventCallback("discard_attachment_send",
                               with_item_id([this](const std::string& id) { OnDiscardAttachmentSend(id); }));
        ctor.BindEventCallback("cancel_attachment_send",
                               with_item_id([this](const std::string& id) { OnCancelAttachmentSend(id); }));
        ctor.BindEventCallback("remove_image", DataEvent([this] { agent_.DiscardPendingAiImage(); }));
        ctor.BindEventCallback("cancel_quote_reply", DataEvent([this] {
                                 if (chat_.editing_message) {
                                   CancelEditMessage(); // the same bar says "Edit message" then
                                 } else {
                                   CancelQuoteReply();
                                 }
                               }));
        ctor.BindEventCallback("jump_to_quote", DataEvent<std::string>([this](const std::string& reply_id) {
                                 if (IsPendingUploadRowId(reply_id)) {
                                   return;
                                 }
                                 JumpToQuotedMessage(reply_id);
                               }));
        ctor.BindEventCallback("open_message_link",
                               DataEvent<std::string, int>([this](const std::string& message_id, int link) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OpenMessageLink(message_id, link);
                               }));
        ctor.BindEventCallback("draft_changed", DataEvent([this] { mentions_.OnDraftChanged(); }));
        ctor.BindEventCallback(
            "open_mention", DataEvent<std::string, int>([this](const std::string& message_id, int run) {
              if (IsPendingUploadRowId(message_id)) {
                return;
              }
              // A highlighted mention: the member's contact page; nothing for someone who is not a contact.
              const auto contact =
                  run < 0 ? std::nullopt : presenter_.MentionedContact(message_id, static_cast<size_t>(run));
              if (contact && contacts_notify_.select_contact) {
                contacts_notify_.select_contact(*contact);
              }
            }));
        ctor.BindEventCallback("open_sender", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OpenGroupSender(message_id);
                               }));
        ctor.BindEventCallback("open_attachment", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OpenAttachment(message_id);
                               }));
        ctor.BindEventCallback("open_picture", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 OpenPicture(message_id);
                               }));
        ctor.BindEventCallback("download_attachment", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 DownloadAttachment(message_id);
                               }));
        ctor.BindEventCallback("retry_attachment", DataEvent<std::string>([this](const std::string& message_id) {
                                 if (IsPendingUploadRowId(message_id)) {
                                   return;
                                 }
                                 RetryAttachmentDownload(message_id);
                               }));
        ctor.BindEventCallback("clear_history", DataEvent([this] { OnClearHistory(); }));
        ctor.BindEventCallback("forget_memory", DataEvent([this] { OnForgetMemory(); }));
        ctor.BindEventCallback("open_search", DataEvent([this] { OnOpenSearch(); }));
        ctor.BindEventCallback("close_search", DataEvent([this] { CloseSearch(/*refocus_composer=*/true); }));
        ctor.BindEventCallback("search_query_changed", DataEvent([this] { OnSearchQueryChanged(); }));
        ctor.BindEventCallback("search_older", DataEvent([this] { OnSearchOlder(); }));
        ctor.BindEventCallback("search_newer", DataEvent([this] { OnSearchNewer(); }));
        ctor.BindEventCallback("open_thread_actions_menu",
                               DataEventAt([this](ui::Event& ev) { menus_.OpenThreadActions(ev); }));
        ctor.BindEventCallback("start_call", DataEventAt([this](ui::Event& ev) { menus_.StartCall(ev); }));
        ctor.BindEventCallback("call_back",
                               DataEvent<std::string>([this](const std::string& video) { menus_.CallBack(video); }));
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
        ctor.BindEventCallback("ai_sessions_search_changed",
                               DataEventAt([this](ui::Event& ev) { OnAiSessionsSearch(ev, /*submit=*/false); }));
        ctor.BindEventCallback("ai_sessions_search",
                               DataEventAt([this](ui::Event& ev) { OnAiSessionsSearch(ev, /*submit=*/true); }));
        ctor.BindEventCallback("toggle_archived", DataEvent([this] { OnToggleArchived(); }));
        ctor.BindEventCallback("reconnect_account", DataEvent([this] { OnReconnectTapped(); }));
        ctor.BindEventCallback("toggle_archived_ai", DataEvent([this] { OnToggleArchivedAi(); }));
        ctor.BindEventCallback("sessions_scroll", DataEvent([this] { OnSessionsScroll(); }));
        ctor.BindEventCallback("ai_sessions_scroll", DataEvent([this] { OnAiSessionsScroll(); }));
        ctor.BindEventCallback("sessions_load_more", DataEvent([this] { OnSessionsLoadMore(); }));
        ctor.BindEventCallback("new_chat", DataEvent([this] { OnNewChat(); }));
        ctor.BindEventCallback(
            "open_new_session_menu", DataEventAt([this](ui::Event& ev) {
              // The phone goes straight to a full-screen page; the desktop keeps its small menu.
              if (ChromeSnapshot().layout_mode == LayoutMode::Compact && contacts_notify_.open_new_chat) {
                contacts_notify_.open_new_chat();
                return;
              }
              menus_.OpenNewSession(ev);
            }));
        ctor.BindEventCallback("select_thread", DataEvent<std::string>([this](const std::string& thread_id) {
                                 OnSelectThread(thread_id);
                               }));
        ctor.BindEventCallback("home_open_brief", DataEvent<int>([this](const int index) { OpenHomeBrief(index); }));
        ctor.BindEventCallback("home_brief_all", DataEvent([this] { SendSuggestionAction("articles"); }));
        ctor.BindEventCallback("home_open_new_people", DataEvent([this] { OpenHomeNewPeople(); }));
        ctor.BindEventCallback("home_open_security", DataEvent([this] {
                                 if (open_settings_section_) {
                                   open_settings_section_("security");
                                 }
                               }));
        ctor.BindEventCallback("home_open_favorite",
                               DataEvent<int>([this](const int index) { OpenHomeFavorite(index); }));
        ctor.BindEventCallback("home_open_calls", DataEvent([this] { OpenHomeCalls(); }));
        ctor.BindEventCallback("home_open_chats", DataEvent([this] { ShellSelectNavTab(NavTab::Sessions); }));
        ctor.BindEventCallback("home_open_mentions", DataEvent([this] { OpenHomeMentions(); }));
        ctor.BindEventCallback("home_edit_open", DataEvent([this] { OpenHomeEdit(); }));
        ctor.BindEventCallback("home_widget_remove", DataEvent<std::string>([this](const std::string& id) {
                                 SaveHomeLayout(RemoveHomeWidget(HomeLayout(), id));
                               }));
        ctor.BindEventCallback("home_widget_add", DataEvent<std::string>([this](const std::string& id) {
                                 SaveHomeLayout(AddHomeWidget(HomeLayout(), id));
                               }));
        ctor.BindEventCallback("home_edit_drag_start",
                               DataEvent<std::string>([this](const std::string& id) { OnHomeEditDragStart(id); }));
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

  shell_setup_.register_pane({.key = "sidebar", .rml_path = "views/sidebar.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane({.key = "contacts", .rml_path = "views/contacts.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane({.key = "settings", .rml_path = "views/settings.rml", .role = PaneRole::Secondary});
  shell_setup_.register_pane({.key = "home", .rml_path = "views/home.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane(
      {.key = "chat", .rml_path = "views/chat.rml", .role = PaneRole::Primary, .provides_composer = true});
  shell_setup_.register_pane(
      {.key = "contact_detail", .rml_path = "views/contact_detail.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane({.key = "new_chat", .rml_path = "views/new_chat.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane({.key = "add_contact", .rml_path = "views/add_contact.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane(
      {.key = "settings_detail", .rml_path = "views/settings_detail.rml", .role = PaneRole::Primary});
  shell_setup_.register_pane({.key = "preview",
                              .rml_path = "views/preview.rml",
                              .role = PaneRole::Auxiliary,
                              .toolbar_label = Tr("shell.pane.preview").c_str()});

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
    StartupPhase phase("ChatController::OnMessagingReady/OnHomeTabActivated");
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
  PollChatShowing();
  ReleaseQueuedTextures();
  // The vault locked: the view cache (previews, pictures, posters, covers) is wiped. Every chat picture becomes its
  // placeholder first (a poster its empty frame, a cover the music note), so no drawing asks for a wiped file (the
  // library would remember that load as failed), then their textures go: the decrypted pixels do not stay in video
  // memory under the PIN page. Unlocked: the rows are drawn again.
  if (messaging_ready_ && !MessagingReady()) {
    if (!vault_locked_seen_) {
      vault_locked_seen_ = true;
      ChatPictureTextures::Locked locked = picture_textures_.DropAllForLock(chat_.messages);
      if (locked.rows_changed) {
        DirtyChatTurns();
      }
      FinishPreviewsProbe();
      preview_batcher_.Clear();
      QueueTextureRelease(std::move(locked.gone));
    }
  } else if (vault_locked_seen_ && messaging_ready_) {
    vault_locked_seen_ = false;
    SyncDisplayFromThread();
    DirtyChatTurns();
  }
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
  // The vault locked under the picture being sent: its plaintext thumbnail goes now (the bubble keeps its place and
  // shows the grey placeholder once drawn again; a retry reads the staged ciphertext, not the thumbnail).
  if (messaging_ready_ && !MessagingReady() && picks_.State().serial != 0) {
    AbandonPick(); // locked: a pick's late results are dropped and released, the entries come back
  }
  if (messaging_ready_ && !MessagingReady()) {
    for (const PendingUpload& item : std::vector<PendingUpload>(pending_uploads_.Queue().Items())) {
      if (item.thumb_path.empty()) {
        continue;
      }
      pending_uploads_.Apply(item.row_id, OnAppLocked);
      if (context_ && context_->GetDocument(0)) {
        PendingUploadRows::DropThumbnail(context_->GetDocument(0), item.row_id); // not drawn again under the lock
      }
    }
    if (pending_uploads_.RefreshTail(chat_.messages)) {
      DirtyChatTurns(); // the placeholder in the row shown too, so no later drawing points at the deleted file
    }
  }
  if (voice_playback_->Active() && now - last_voice_call_check_ >= std::chrono::milliseconds(250)) {
    last_voice_call_check_ = now;
    if (facade_ && facade_->AnyCallLive()) {
      voice_playback_->OnCallLive();
    }
  }
}

bool ChatController::ChatShowing() const {
  if (!messaging_ready_ || ActiveThreadId().empty()) {
    return false;
  }
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  return chrome.primary_pane_key == "chat" && (chrome.layout_mode != LayoutMode::Compact || chrome.compact_chat_open);
}

void ChatController::PollChatShowing() {
  const bool showing = ChatShowing();
  if (showing == chat_showing_) {
    return;
  }
  chat_showing_ = showing;
  if (!showing) {
    FinishPreviewsProbe();
    preview_batcher_.Clear(); // a redraw already scheduled finds nothing for rows no longer shown
    if (facade_) {
      facade_->CancelChatPreviews(); // only the chat's requests not started; the storage grid's stay
    }
    ScheduleChatWindowMetric(std::chrono::seconds(5), /*exited=*/true);
    // Left on its newest messages, the chat opens again from its first screen (not a vault lock: the PIN page over it).
    if (MessagingReady()) {
      scroller_.OnChatLeft();
    }
    // The chat's picture textures go once it is off screen (after the page's slide-out: a texture taken while its row
    // is still drawn is loaded again at once). Coming back first (FinalizeThreadDisplay) voids it.
    const uint64_t turn = chat_exit_turn_;
    AppRuntime::ScheduleCoordinatorOneShot(std::chrono::seconds(1),
                                           [this, turn, alive = std::weak_ptr<int>(chat_window_alive_)]() {
                                             AppRuntime::PostUI([this, turn, alive]() {
                                               if (!alive.lock() || turn != chat_exit_turn_ || ChatShowing()) {
                                                 return;
                                               }
                                               QueueTextureRelease(picture_textures_.UnmountAll());
                                             });
                                           });
  }
}

void ChatController::SyncPictureTextures() {
  QueueTextureRelease(picture_textures_.Mount(chat_.messages));
}

void ChatController::QueueTextureRelease(std::vector<std::string> srcs) {
  if (srcs.empty()) {
    return;
  }
  textures_release_queued_.insert(textures_release_queued_.end(), std::make_move_iterator(srcs.begin()),
                                  std::make_move_iterator(srcs.end()));
  if (context_) {
    context_->RequestNextUpdate(0.0); // released two frames on, also in an idle chat
  }
}

void ChatController::ReleaseQueuedTextures() {
  // Queued in an earlier frame, so the page has been updated since (Context::Update applied the rows): no element
  // shows these files any more, and a texture taken from a drawn element would only be loaded again.
  if (!textures_release_ready_.empty()) {
    size_t called = 0;
    size_t hit = 0;
    for (const std::string& src : textures_release_ready_) {
      if (picture_textures_.Count(src) > 0) {
        continue; // back on the page meanwhile
      }
      ++called;
      hit += ui::ReleaseTexture(TextureKeyForImageSrc(src)) ? 1 : 0;
    }
    if (called > 0) {
      // No path: how many were asked and how many were loaded (a texture never drawn is not one).
      log().info << "chat picture textures released: called=" << called << " hit=" << hit;
    }
  }
  textures_release_ready_ = std::exchange(textures_release_queued_, {});
  if (!textures_release_ready_.empty() && context_) {
    context_->RequestNextUpdate(0.0);
  }
}

void ChatController::OnChatPreviewDone(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                       const bool made) {
  if (!messaging_ready_ || thread_id != ActiveThreadId() || !ChatShowing()) {
    // Another chat's, or this chat left (its textures already went): nothing is drawn into rows not shown and nothing
    // is counted; the preview file stays and the next drawing of the rows shows it.
    return;
  }
  const std::string hex = AttachmentHashHex(content_hash);
  if (!made) {
    previews_failed_.insert(hex); // a file card now: the card changes, not only its picture
  }
  const auto delay = preview_batcher_.Add(hex, std::chrono::steady_clock::now());
  if (!delay) {
    return; // a redraw is coming
  }
  auto redraw = [this, alive = std::weak_ptr<int>(chat_window_alive_)]() {
    if (alive.lock()) {
      RedrawPreviews();
    }
  };
  if (delay->count() == 0) {
    AppRuntime::PostUI(std::move(redraw));
  } else {
    AppRuntime::ScheduleCoordinatorOneShot(*delay, [redraw = std::move(redraw)]() { AppRuntime::PostUI(redraw); });
  }
}

void ChatController::RedrawPreviews() {
  const auto now = std::chrono::steady_clock::now();
  const std::vector<std::string> ready = preview_batcher_.Take(now);
  if (ready.empty() || !messaging_ready_ || !facade_ || ActiveThreadId().empty() || !ChatShowing()) {
    previews_failed_.clear(); // left meanwhile: the rows are drawn afresh when shown again
    return;
  }
  const std::string thread_id = ActiveThreadId();
  const TranscriptFacts facts = facade_->TranscriptFactsFor();
  const ReadyPreviews applied =
      ApplyReadyPreviews(chat_.messages, ready, previews_failed_,
                         [&](const std::string& message_id) -> std::optional<TranscriptAttachmentView> {
                           auto message = facade_->GetMessage(thread_id, message_id);
                           if (!message || !*message || !facts.attachment) {
                             return std::nullopt;
                           }
                           return facts.attachment(**message);
                         });
  const bool whole = applied.redraw_all;
  const bool changed = applied.changed;
  bool last = false;
  for (const std::string& hex : ready) {
    last = previews_probe_.Done(hex, previews_failed_.contains(hex), now) || last;
  }
  previews_failed_.clear();
  if (whole) {
    SyncDisplayFromThread(); // rare (a preview that cannot be made): the rows drawn again, data-rml resets the changed
  } else if (changed) {
    SyncPictureTextures();
  }
  if (whole || changed) {
    DirtyChatTurns();
  }
  if (last) {
    FinishPreviewsProbe();
  }
}

void ChatController::FinishPreviewsProbe() {
  if (!previews_probe_.Active()) {
    return;
  }
  const ChatPreviewsProbe::Report report =
      previews_probe_.Finish(std::chrono::steady_clock::now(), UiFrameStats::SlowFrames());
  const CurrentMemoryMb memory = ReadCurrentMemoryMb();
  MetricsLine("chat.previews")
      .Add("count", report.count)
      .Add("failed", report.failed)
      .Add("ms", report.ms)
      .Add("visible_ms", report.visible_ms)
      .Add("slow_frames", report.slow_frames)
      .Add("complete", report.complete)
      .Add("footprint_mb", memory.footprint_mb)
      .Add("rss_mb", memory.rss_mb)
      .Emit();
}

void ChatController::MeasureVisiblePreviews() {
  if (!previews_probe_.Active() || previews_probe_.VisibleKnown() || !scroller_.Settled() || !context_ ||
      context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* document = context_->GetDocument(0);
  ui::Element* scroll = document->GetElementById("chat-messages");
  if (!scroll || scroll->GetClientHeight() <= 0.f) {
    return;
  }
  const float top = scroll->GetAbsoluteOffset(ui::BoxArea::Padding).y;
  const float bottom = top + scroll->GetClientHeight();
  ui::ElementList placeholders;
  document->GetElementsByClassName(placeholders, "chat-picture-placeholder");
  std::vector<std::string> on_screen;
  for (ui::Element* element : placeholders) {
    const float y = element->GetAbsoluteOffset(ui::BoxArea::Border).y;
    const float h = element->GetBox().GetSize(ui::BoxArea::Border).y;
    if (y < bottom && y + h > top) {
      on_screen.push_back(element->GetAttribute<ui::String>("pic-hash", ""));
    }
  }
  previews_probe_.SetVisible(on_screen, std::chrono::steady_clock::now());
}

void ChatController::ScheduleChatWindowMetric(const std::chrono::milliseconds delay, const bool exited) {
  uint64_t& counter = exited ? chat_exit_turn_ : chat_paged_turn_;
  const uint64_t turn = ++counter;
  AppRuntime::ScheduleCoordinatorOneShot(delay, [this, turn, exited, alive = std::weak_ptr<int>(chat_window_alive_)]() {
    AppRuntime::PostUI([this, turn, exited, alive]() {
      if (!alive.lock() || turn != (exited ? chat_exit_turn_ : chat_paged_turn_)) {
        return;
      }
      const bool showing = ChatShowing();
      if (showing == exited) {
        return; // left and came back (or paged and left): the reading would not say what it is named for
      }
      const CurrentMemoryMb memory = ReadCurrentMemoryMb();
      ChatWindowLine(chat_.messages, exited, exited ? std::nullopt : scroller_.TakePageReading(), memory).Emit();
    });
  });
}

void ChatController::OnSyncLayoutMeasured(const int64_t ms) {
  if (!open_probe_.layout_pending) {
    return;
  }
  open_probe_.layout_pending = false;
  const auto started = std::chrono::steady_clock::now() - std::chrono::milliseconds(ms);
  if (started - open_probe_.built_at > std::chrono::seconds(2)) {
    return; // not the layout that followed this open
  }
  const CurrentMemoryMb memory = ReadCurrentMemoryMb();
  MetricsLine("chat.open")
      .Add("rows", open_probe_.rows)
      .Add("pictures", open_probe_.pictures)
      .Add("placeholders", open_probe_.placeholders)
      .Add("build_ms", open_probe_.build_ms)
      .Add("layout_ms", ms)
      .Add("first_screen", ChatTranscriptScroller::FirstScreenWord(open_probe_.first_screen))
      .Add("footprint_mb", memory.footprint_mb)
      .Add("rss_mb", memory.rss_mb)
      .Emit();
}

void ChatController::AfterLayout() {
  if (search_focus_ != SearchFocus::None && context_ && context_->GetNumDocuments() > 0) {
    const char* id = search_focus_ == SearchFocus::Input ? "search-input" : "draft-input";
    search_focus_ = SearchFocus::None;
    if (ui::Element* el = context_->GetDocument(0)->GetElementById(id)) {
      el->Focus();
    }
  }
  SyncComposerOverlay();
  scroller_.ApplyPolicy();
  MeasureVisiblePreviews();
  FillSessionListAfterLayout();
}

namespace {

/** Sets `name` to `dp` unless it already is (no relayout for an unchanged value). */
void SetDpIfChanged(ui::Element* element, const char* name, const float dp) {
  if (!element) {
    return;
  }
  if (const ui::Property* now = element->GetLocalProperty(name)) {
    const ui::NumericValue value = now->GetNumericValue();
    if (value.unit == ui::Unit::DP && std::abs(value.number - dp) < 0.5f) {
      return;
    }
  }
  element->SetProperty(name, std::to_string(static_cast<int>(std::lround(dp))) + "dp");
}

ui::Element* FirstByClass(ui::Element* root, const char* name) {
  ui::ElementList found;
  root->GetElementsByClassName(found, name);
  return found.empty() ? nullptr : found.front();
}

} // namespace

void ChatController::SyncComposerOverlay() {
  if (context_ && context_->GetNumDocuments() > 0) {
    KeepClearOfComposer(context_->GetDocument(0)->GetElementById("shell-chat-overlay"));
  }
}

void ChatController::KeepClearOfComposer(ui::Element* overlay) {
  // A phone chat's composer is a clear bar with a floating pill over the transcript (TODO #202), which runs down under it: the
  // transcript's end spacer, its fade and the "new messages" pill follow the bar's height (one line or five, a
  // picture chip, a quote, the home indicator or the keyboard), so the newest message and the pill stay above it.
  ui::Element* mount = overlay ? overlay->GetElementById("shell-composer-mount") : nullptr;
  ui::Context* context = mount ? mount->GetContext() : nullptr;
  if (!context) {
    return;
  }
  const float ratio =
      context->GetDensityIndependentPixelRatio() > 0.f ? context->GetDensityIndependentPixelRatio() : 1.f;
  const float bar_dp = mount->GetOffsetHeight() / ratio;
  if (bar_dp <= 0.f) {
    return;
  }
  SetDpIfChanged(FirstByClass(overlay, "composer-clear"), "height", bar_dp + kComposerClearGapDp);
  SetDpIfChanged(FirstByClass(overlay, "chat-composer-fade"), "bottom", bar_dp);
  SetDpIfChanged(FirstByClass(overlay, "chat-jump-latest"), "bottom", bar_dp + kJumpLatestGapDp);
}

void ChatController::Shutdown() {
  voice_->Shutdown();
  voice_playback_->Shutdown();
  AppLifecycle::RemoveListener(lifecycle_background_id_);
  lifecycle_background_id_ = 0;
  AppLifecycle::RemoveListener(lifecycle_foreground_id_);
  lifecycle_foreground_id_ = 0;
  if (send_mark_timer_ != 0) {
    AppRuntime::CancelCoordinatorTimer(send_mark_timer_);
    send_mark_timer_ = 0;
  }
  ++send_mark_redraw_turn_; // a firing already posted does nothing
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
  pending_uploads_.Drop(); // the lock check never sees this teardown: the plaintext thumbnail goes here
  AbandonPick();
  video_bubble_thread_.clear();
  messaging_ready_ = false;
  ++messaging_session_; // a task result still on its way is dropped
  if (ready_tasks_) {
    ready_tasks_->OnMessagingTornDown();
  }
  ResetRecovery();
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

void ChatController::AskAiOpenEmoji(std::function<void(const std::string& emoji)> insert) {
  if (emoji_picker_notify_.open_insert_into) {
    emoji_picker_notify_.open_insert_into(std::move(insert));
  }
}

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
  // Phones, a 1:1 chat: straight to their page (DEV/PLAN-fewer-taps-1.md 2); the menu stays elsewhere.
  if (peer_sheet_.OpenPeerPage(AskAiCompact())) {
    return;
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
    action.detail =
        Tr(preset == GroupPreset::Managed ? "group_info.preset_managed_detail" : "group_info.preset_open_detail");
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
      thread_id,
      [this, thread_id, group_id, identity]() { return presenter_.HandOverAndClose(thread_id, group_id, identity); },
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
