#pragma once

#include "feature/ai/AgentUiPorts.h"
#include "gui/contacts/ContactsNotifyPorts.h"
#include "gui/contacts/PeoplePickerNotifyPorts.h"
#include "gui/EmojiPickerNotifyPorts.h"
#include "gui/UpdateCard.h"
#include "feature/presenters/ChatPresenter.h"
#include "feature/presenters/DeviceRemovedNotice.h"
#include "feature/presenters/HomeBrief.h"
#include "feature/presenters/HomeToday.h"
#include "feature/presenters/NewsReader.h"
#include "feature/presenters/ThreadSearchSession.h"
#include "feature/conversations/ConversationsFacade.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/PlatformShareFile.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/AskAiPanel.h"
#include "foundation/runtime/BackgroundActivity.h"
#include "gui/chat/AttachmentPreparing.h"
#include "gui/chat/CameraCaptureFlow.h"
#include "gui/chat/OutgoingAttachmentPrep.h"
#include "gui/chat/PendingUploadRows.h"
#include "gui/chat/PictureSendController.h"
#include "feature/presenters/AttachmentPicks.h"
#include "feature/presenters/PictureSendQueue.h"
#include "feature/presenters/VoiceSendQueue.h"
#include "gui/chat/PhotoPreview.h"
#include "gui/chat/ChatOpenMetrics.h"
#include "gui/chat/ChatPictureTextures.h"
#include "gui/chat/PreviewRedrawBatcher.h"
#include "gui/chat/RegistrationRecovery.h"
#include "gui/chat/SendFailureFix.h"
#include "gui/chat/VoiceComposer.h"
#include "gui/chat/VoicePlayback.h"
#include "gui/chat/ChatMentionPicker.h"
#include "gui/chat/ChatMenus.h"
#include "gui/chat/ChatPeerSheet.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatThreadChrome.h"
#include "gui/chat/GroupInfoSheet.h"
#include "gui/chat/GroupJoinPrompt.h"
#include "gui/chat/GroupLinkSheet.h"
#include "gui/chat/GroupLeaveSheet.h"
#include "gui/chat/ChatTranscriptScroller.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/chat/HeldAgentSend.h"
#include "gui/chat/MessagingReadyTasks.h"
#include "gui/chat/SessionListWindow.h"
#include "gui/chat/ThreadDrafts.h"
#include "gui/chat/WorkingSetController.h"
#include "feature/conversations/MessagingUiPorts.h"
#include "gui/BadgeNotifyPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/CallActionsPorts.h"
#include "gui/ChatSurfaceNotifyPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "gui/shell/ShellSetupPorts.h"
#include "gui/UnlockEnsurePorts.h"
#include "lib/messaging/AtAiParser.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/TurnPlan.h"
#include "foundation/data/Config.h"
#include "foundation/data/SessionStore.h"
#include "common/Module.h"
#include "lib/ui/ChatWidgetTypes.h"
#include "lib/ui/ContextMenuHost.h"

#include <source_location>
#include <ui/base/ObserverPtr.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Event.h>
#include <ui/base/Input.h>
#include <ui/base/Types.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <unordered_map>
#include <functional>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
class Element;
} // namespace ui

namespace pbr {

class InputCoordinator;

/** One chats-list row as the RML binds it (letter avatar, or the person's photo when `icon_path` is set). */
SessionDisplayRow ToSessionDisplayRow(const ChatSessionRow& from);

class ChatController : public Module,
                       private AgentTurnHost,
                       private ChatMenusHost,
                       private AskAiPanelHost,
                       private GroupInfoSheetHost,
                       private GroupLeaveSheetHost {
public:
  using AgentConfig = AgentTurnController::AgentConfig;

  static AgentConfig ProjectAgent(const AppConfig& config) { return AgentTurnController::ProjectAgent(config); }
  /** Phones: keeps `overlay`'s (#shell-chat-overlay) transcript end spacer, fade and "new messages" pill clear of its
   *  floating composer bar, whatever the bar's height. Per frame; changes nothing when they already are. */
  static void KeepClearOfComposer(ui::Element* overlay);
  /** Between the newest message and the composer bar; between the "new messages" pill and the bar. */
  static constexpr float kComposerClearGapDp = 12.f;
  static constexpr float kJumpLatestGapDp = 16.f;
  /** A conversation row is kept by a list's search when its title or preview contains the (lower-cased) query. */
  static bool SessionRowMatches(const std::string& title, const std::string& preview, const std::string& lowered_query);
  void Apply(const AgentConfig& config) { agent_.ApplyConfig(config); }
  void BindAgentPorts(AgentUiPorts ports) { agent_.BindPorts(std::move(ports)); }
  void ReloadAgentConfig() { agent_.ReloadAgentConfig(); }
  void ReloadAgentConfigWhenIdle() { agent_.ReloadAgentConfigWhenIdle(); }
  /** Binds this device's push tokens off the UI thread (one run at a time; MessagingReadyTasks). */
  void SyncPushDevices();
  /** The Brief key's mint run, shared with the 401 renewal (B183, Application). */
  const std::shared_ptr<CoalescedRun>& BriefKeyRun() const { return agent_.BriefKeyRun(); }
  /** A 401 renewal held the Brief key run while a mint was asked for: ask again (does nothing once a key is stored). */
  void RetryBriefKeyMint();

  /**
   * A peer's photo was fetched (new path: files are named after their bytes). Rebuilds what shows photos: the open
   * transcript's sender avatars, the chat header, the chats list, Home's widgets and an open group info sheet.
   */
  void OnPeerIconsChanged();
  /** An imported account's first sync is running / late (AccountSyncState): the empty Chats list shows it. */
  void SetAccountSync(bool syncing, bool delayed);

  ChatController();
  ~ChatController() override = default;

  using SessionRow = SessionDisplayRow;

  bool Setup(ui::Context* context);
  /** Non-owning; pass nullptr to clear. Rebinds sub-presenters (scroller/chrome). */
  void BindConversationsFacade(ConversationsFacade* facade);
  /** Shown messages a chat of `kind` opens with (the scroller's first screen): a group more than a direct or AI chat. */
  static size_t FirstScreenMessages(ThreadKind kind);
  /**
   * G021: `text` holds an invite link (`pp://join/…`, opened, pasted or scanned): asks to join by it — the preview,
   * then the group's chat — and returns true. False: not one (the caller tries a contact locator).
   */
  bool OfferGroupJoin(const std::string& text);
  /** App-wired hook so messaging tool registration stays in Application (no ui→hub registration edge). */
  void BindRegisterMessagingTools(std::function<void(ToolRegistry&)> hook);
  void BindContactsNotify(ContactsNotifyPorts ports);
  void BindPeoplePickerNotify(PeoplePickerNotifyPorts ports);
  void BindEmojiPickerNotify(EmojiPickerNotifyPorts ports);
  /** Append a normalized emoji glyph to the composer draft (used by emoji picker).
   *  @param restore_composer_focus When true (popover), focus the composer after insert.
   *         When false (keyboard panel), advance caret only so the OSK stays dismissed. */
  void InsertEmojiIntoDraft(const std::string& emoji, bool restore_composer_focus = true) override;
  /** Toggle a reaction from the emoji picker (public for Application wiring). */
  void ReactWithEmoji(const std::string& message_id, const std::string& emoji);
  void BindShellSetup(ShellSetupPorts ports);
  void BindSessionStore(SessionStore& store);
  void BindBadgeNotify(BadgeNotifyPorts ports);
  void BindInputCoordinator(InputCoordinator& input);
  void BindCallActions(CallActionsPorts ports);
  /** Messaging and the mesh are ready: the header's call button shows on exactly this (ChatThreadChrome). */
  bool PeerCallsReady() const { return messaging_ready_ && mesh_ready_; }
  /** Blocks someone who is not a contact: saved as a blocked contact; the new contact's id (or why not). */
  Roe<std::string> BlockStranger(const std::string& account_id) { return presenter_.BlockStranger(account_id); }
  /** Adds someone who is not a contact from the local directory copy and hands them the chat (the old menu's Add). */
  Roe<std::string> AddStranger(const std::string& account_id) { return presenter_.AddPeerToContacts(account_id); }
  /** Refresh peer-link / "In call" header (Application wires from CallController). */
  void UpdatePeerLinkChrome();
  void BindUnlockEnsure(UnlockEnsurePorts ports);
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Push surface snapshot to app ChatShellBridge. Clear via BindSurfaceNotify({}). */
  void BindSurfaceNotify(ChatSurfaceNotifyPorts ports);
  void BindMessagingUi(MessagingUiPorts ports);
  SessionStore& Store() override;
  const SessionStore& Store() const;
  void Update();
  /** Call after ui::Context::Update so follow-tail uses fresh layout heights. */
  void AfterLayout();
  /** A shell SyncLayout finished in `ms`: the one after a chat was opened completes its chat.open metric. */
  void OnSyncLayoutMeasured(int64_t ms);
  void Shutdown();
  void OnApplicationPause();
  /** The vault locked, or another tab was opened: a recording is thrown away and a clip stops. */
  void CancelVoice();
  void FinalizeThreadDisplay();
  /**
   * An attachment sent into `thread_id` from outside this page (Settings → Send run logs) finished or failed: when that
   * chat is the open one its rows and its Retry chip are read again.
   */
  void RefreshAfterOutsideSend(const std::string& thread_id);
  /** Sends `text` in the open chat as if the user typed it (a settings "?" asking PP 客服). */
  void SendTypedText(const std::string& text) { SendUserText(text); }
  /** Put `text` in the open chat's composer and focus it; the user sends it (Settings "?" → PP Support). */
  void PrefillDraft(const std::string& text);
  void OnHomeTabActivated();
  /** Edit Home (DESIGN-pp-ui 8.16): the Home widgets on and off Home; Home's "Edit" and Me → Appearance. */
  void OpenHomeEdit();
  /** A tap on a message's emoji: the sheet that lists who reacted (grouped by emoji). */
  void OpenReactors(const std::string& message_id);
  /** The profile preferences changed (a contact starred, Edit Home): Home's widgets are drawn again. */
  void OnHomePrefsChanged();
  /** The Chats page card "save your recovery phrase" (RecoveryReminder.h); "Later" counts toward its one repeat. */
  bool RecoveryReminderShows();
  void OnRecoveryReminderLater();
  /**
   * The Chats page card "a new version is available" (UpdateCard.h), under the recovery card: Update opens the link
   * (when there is a usable one), "Later" hides it on the recovery card's rhythm. RefreshUpdateCard draws it anew
   * (the advice changed).
   */
  void BindUpdateCard(UpdateCardPorts ports);
  bool UpdateCardShows();
  void RefreshUpdateCard();
  void OnUpdateCardOpen();
  void OnUpdateCardLater();
  /**
   * The Chats page notice that this device was taken off its account (DeviceRemovedNotice): checked at every messaging
   * start and whenever the device list changes; "Link again" hands over to the reset flow, "OK" hides it this launch.
   */
  void BindDeviceRemovedNotice(DeviceRemovedNoticePorts ports);
  bool DeviceRemovedNoticeShows() const { return device_removed_.Shows(); }
  void OnSessionsTabActivated();
  /**
   * Phone: back from Settings / Contacts to the chat overlay left open under them (ShellHost
   * SetOnCompactChatShownAgain). What arrived meanwhile was not on screen; the chat is read now, refreshed and at
   * its newest message, as when it is opened.
   */
  void OnCompactChatShownAgain();
  /**
   * The user may be looking at the app again (phone foreground, desktop restored / focus regained; UI thread). A
   * message that came in while nobody looked counted unread even in the open chat: it is read now (a refresh
   * follows only when it was).
   */
  void OnAttentiveAgain();
  void OnFindSomeone() override;
  /**
   * The phone "New chat" page's "AI chat": a new AI chat, opened. Being the newest, it is the one the ask-AI panel
   * continues from then on (DEV/PLAN-ui-batch5-ask-ai.md 5a).
   */
  void StartNewAiChat() { OnNewChat(); }
  /** A setting's "?" on a phone: the sheet shows the PP 客服 chat and asks `question` there. False elsewhere. */
  bool AskSupportPanel(const std::string& question) { return ask_ai_.OpenSupport(question); }
  /** The PP 客服 chat found or made, not opened (Application's EnsureSupportThread), for the sheet's PP 客服 mode. */
  void BindEnsureSupport(std::function<Roe<std::string>()> ensure_support) {
    ensure_support_ = std::move(ensure_support);
  }
  /** Home's Security widget opens Me → that page (SettingsController::OpenSection). */
  void BindOpenSettingsSection(std::function<void(const std::string& section_id)> open) {
    open_settings_section_ = std::move(open);
  }
  /** The reconnect row and alert register again through Me → Profile's action (SettingsController::ReconnectAccount). */
  void BindReconnectAccount(std::function<void(std::function<void(bool ok)> done)> reconnect) {
    reconnect_account_ = std::move(reconnect);
  }
  /**
   * A failed message's "update their card": the contact page's refresh (contacts_ports_.refresh_contact, one hub call),
   * `done` once on the UI thread; false when one already runs for that contact.
   */
  void BindRefreshContact(
      std::function<bool(const std::string& contact_id, std::function<void(const ContactRefreshResult&)> done)>
          refresh) {
    refresh_contact_ = std::move(refresh);
  }
  /**
   * Runs `run` (a registration the user asked for) now, or, while the automatic renewal is out, once it ends
   * ("Reconnecting, please wait."), so the two never overlap. `run` holds the renewal's slot until it calls the
   * `release` it is given, once, on every way out. UI thread.
   */
  void RunWhenRegistrationIdle(std::function<void(std::function<void()> release)> run);
  /** Messaging was torn down (reset): frees the renewal slot a registration still holds. Application calls it. */
  void OnMessagingTornDownForRegistration();
  /** The panel's sheet is a modal layer: the coordinator says when the shell closed it. */
  void BindFlowCoordinator(FlowCoordinatorPorts ports) {
    picture_send_.BindFlowCoordinator(ports);
    flow_coordinator_ = std::move(ports);
  }
  // By value, not by reference: Home callers pass a reference into home_favorites_ / home_summary_, and selecting
  // from Home first switches to the Chats tab, which rebuilds those containers -- the reference would dangle before
  // SelectThread hashes it (B161: SIGSEGV in PrefsOf's hash lookup, phones only).
  void OnSelectThread(std::string thread_id);
  /**
   * A notification tap's chat (a banner's, or the one a message alert's message synced into): opened with nothing
   * over it (ShellHost::ClearForTappedChat); then the tap's cover goes once the chat is selected. With the tap's cover
   * armed: messaging not ready yet keeps the cover (bounded by its hold) for the later route; a chat that is gone (or
   * cannot open) shows the chat list. Without the cover: only the selection, as before (a missing chat does nothing). A
   * ring or call screen up: only the selection (the call keeps its screen).
   */
  void OpenTappedThread(std::string thread_id);
  /**
   * A fresh message / call alert tap whose message already synced (before the tap, or before this wiring): its chat
   * opens now instead of waiting for a sync that already happened. Messaging ready only.
   */
  void RouteSyncedTap();
  /** Rebind messaging ports after a full profile data wipe/reinit. */
  void OnProfileDataReset();
  /** Called when messaging becomes ready (app-wired). */
  void OnMessagingReady();
  void OnMeshReady();
  /** App-wired from ShellHost layout sync (see Application::WireShellPresentationEvents). */
  void OnShellLayoutSynced();
  /** App-wired from ShellHost: the window changed size (a rotation). */
  void OnViewportResized();
  /** Phone Chats list swipe: runs the same item the long-press menu offers. */
  void OnSessionSwipeAction(const std::string& thread_id, SessionSwipeAction action);
  /** A tap on the phone AI dot: the ask-AI panel over the current page (DESIGN-pp-ui §8.14). */
  void OpenAskAiFromDot() { (void)ask_ai_.Open(AskAiEntry::Dot); }
  /** Escape after the shell closed nothing (desktop). Not in the composer: NotInComposer. */
  enum class ComposerEscape { NotInComposer, Handled, Idle };
  ComposerEscape OnComposerEscape();
  /** The composer shows the unsent text of the conversation now open (Home has its own). App-wired to every
   * nav-tab change, and called after every display sync. */
  void SyncDraftOwner();

private:
  using ChatState = ChatScreenState;

  using ShellState = ChatShellState;

  struct PendingReply {
    std::string entry_id;
    std::string thread_id;
    std::string output;
    bool from_llm = false;
  };

  /** Events both data models ("chat" and "shell") bind: the agent's widgets, chat actions, "new message". */
  void BindSharedEvents(ui::DataModelConstructor& ctor);
  void OnSendMessage();
  /** A Home suggestion that runs a function (`id`): its localized sentence is sent with the function's payload. */
  void SendSuggestionAction(const std::string& id);
  void OnNewChat() override;
  void OnNewMessage() override;
  void OnCloseThread(const std::string& thread_id) override;
  void OnClearHistory();
  /** Asks, then deletes the thread's messages on this device (the thread stays in the list). */
  void ClearHistoryOf(const std::string& thread_id) override;
  void AfterHistoryCleared(const std::string& thread_id);
  /** After a chat's settings changed (the presenter refreshed its list): badges and the surface follow. */
  void AfterThreadPrefsChanged() override;
  /** G016: asks once, then the owner dissolves the group for everyone (the chat stays, read-only). */
  void DissolveGroupAfterConfirm(const std::string& group_id);
  /** Run a close (`action`) and, when it went through, clear what the open chat held; tells the user
   *  when it failed (`show_error_text`: the action's own message, else "could not delete"). */
  void CloseChatWith(const std::string& thread_id, const std::function<Roe<void>()>& action,
                     bool show_error_text = false);
  /** Copy the presenter's chats list into the shell's data model. */
  void ApplySessionList();
  /** The phone Home widgets' data from the chats list (Recent chats, Mentions, New people, Favorites). */
  void ApplyHomeToday(const ChatListScreen& list);
  /** Which Home widgets show and in which order, the calls' and briefs' lines, and Edit Home's rows. */
  void ApplyHomeWidgets();
  /** Favorites: starred contacts (the star order) then recent chats with contacts; a starred one is looked up here. */
  void ApplyHomeFavorites(const std::vector<ChatSessionRow>& rows);
  /** Sizes the 关注联系人 avatars from the window width (four across the card); dirties them when they change. */
  void SyncHomeFavoriteSize();
  /** Phones, Home shown, Calls kept: reads the newest calls on a worker and draws the Calls widget. */
  void RefreshHomeCalls();
  /**
   * Phones, Home shown: reads the profile's briefs cache once and, when it is over a day old (or for another
   * language), fetches the article feed again on a worker (DESIGN-pp-ui 8.16).
   */
  void RefreshHomeBrief();
  /** The article feed of the UI language ("cn" / "en"). */
  static std::string HomeBriefFeed();
  /** A tap on a headline: its article through the usual open-link confirm; with no link, the feed in the AI chat. */
  void OpenHomeBrief(int index);
  /** A tap on the "New people" line: the first chat with unread messages from someone who is not a contact. */
  void OpenHomeNewPeople();
  /** A favourite's avatar: their newest chat, or (none yet) a new one as the contact page's Message starts. */
  void OpenHomeFavorite(int index);
  /** The Calls widget: the chat of the newest call. */
  void OpenHomeCalls();
  /** The Mentions widget: the chat, at its oldest unread mention. */
  void OpenHomeMentions();
  /** The widgets the user keeps on Home and their order (profile preferences; device-only). */
  HomeWidgetLayout HomeLayout() const;
  /** Saves an Edit Home change, redraws Home, and reads / fetches what a widget put back needs. */
  void SaveHomeLayout(const HomeWidgetLayout& layout);
  /**
   * Edit Home's drag handles: a press on `id`'s handle lifts its row; while the finger moves the row takes the place
   * of the row nearest the finger (the list redraws in that order); let go, the order is saved.
   */
  void OnHomeEditDragStart(const std::string& id);
  void OnHomeEditDrag(ui::Event& event);
  void OnHomeEditDragEnd();
  /** Edit Home's rows from `layout` (the drag's working order while a handle is held). */
  void ApplyHomeEditRows(const HomeWidgetLayout& layout);
  void OnToggleArchived();
  void OnToggleArchivedAi();
  void OnSessionsSearchChanged();
  /** The AI drawer's search: typing filters live; Return or the magnifier also puts the keyboard away. */
  void OnAiSessionsSearch(ui::Event& ev, bool submit);
  void OnForgetMemory() override;
  void OnSyncWithPeer() override;
  void OnRetryGapSync();
  void OnStartNewSecureChat();
  void OnPauseIntegrityOnly();
  void OnCopyPskKey();
  void OnTogglePskImport();
  void OnImportPsk();
  void OnVerifyPsk();
  void OnRotatePskExport();
  void OnLockPublicToThisDevice() override;
  /** Opens the AI thread a send from Home goes to: `chip_message`'s own earlier thread when there is one, else a new thread. */
  bool EnsureHomeOutboundSession(const std::string& chip_message = {}) override;
  /** `from_home_chip`: a Home chip (not typed text) is being sent, so it continues that chip's own thread. */
  void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt,
                    bool from_home_chip = false) override;
  void ToggleReaction(const std::string& message_id, const std::string& emoji) override;
  std::vector<std::string> RecentEmojis() const override;
  void RememberEmoji(const std::string& emoji) override;
  void OnAttachFile(ui::Event* ev = nullptr);
  /** The camera icon: iOS opens the system camera at once; Android first asks "Take a photo / Record a video". */
  void OnAttachCapture(ui::Event* ev = nullptr);
  void OpenCameraCapture(CameraCaptureKind kind);
  /** `pick_serial`: the camera's pick (a video sent from it carries it on until its bubble). */
  CaptureFlowPorts MakeCaptureFlowPorts(uint64_t pick_serial, const std::string& thread_id);
  /** Recompute `show_capture` (call, upload, chat kind); dirty only on a change. */
  void RefreshShowCapture();
  /** After a picture is picked: "Send" (shrunk), "Send original", Cancel. */
  void ShowPictureSendChoice(std::string path, const std::string& thread_id);
  void ToggleVoiceMode();
  float VoiceTopLimitDp() const;
  float VoiceOverlayTopDp() const;
  /** A video was accepted and is being copied out by the system (iOS): the chip says "Sending…" until it arrives. */
  void BeginAttachmentPreparing(const std::string& name);
  /** Every way out of that wait (path arrived, nothing came, cancelled, failed) ends it. */
  void EndAttachmentPreparing();
  /** Mirror preparing_ into the chat fields (the one place chat_.attachment_preparing is written). */
  void SyncAttachmentPreparing();
  /** chat_.attachment_here: an attachment is in flight (or failed) in the chat that is open now. */
  void RefreshAttachmentHere();
  /**
   * Sends `path` to `to_thread` and only there: the chat a pick was armed in, a recording's own (never whatever
   * chat is open when this runs; empty: nothing is sent). `voice_file`: a voice recording of ours; it is deleted once
   * the send succeeded or is staged for a retry, otherwise it is kept and the chip offers Retry / Discard.
   * `reuse_row`: a voice message from voice_queue_, whose bubble was made when the recording was finished.
   * `on_finished` hears how the send ended exactly once, on every way out (AttachmentSendOnce; empty: nobody asks).
   * `from_picture_queue`: an item of picture_queue_, which holds the attachment slot between its items, so only a send
   * really running refuses it; its file is the queue's (released by the queue, never here), it has no pick, a pace
   * refusal says nothing (the queue tries again), and a failed bubble does not give way to it.
   */
  void StartAttachmentUpload(const std::string& path, OutgoingPickSource source, bool voice_file,
                             const std::string& to_thread, std::optional<PendingUploadKind> bubble = std::nullopt,
                             uint64_t pick_serial = 0, std::function<void(AttachmentSendOutcome)> on_finished = {},
                             bool from_picture_queue = false, const std::string& reuse_row = {});
  /**
   * The one "an attachment is on its way" check: preparing (a pick being copied), uploading, or a picture queue that
   * has started and not ended (its gaps and pace waits included). Every entry that starts an attachment, and every
   * entry greyed meanwhile, asks this.
   */
  bool AttachmentBusy() const;
  /**
   * A queue for the pictures of one pick in `thread_id` (DEV/PLAN-picture-send-preview.md), wired to
   * StartAttachmentUpload, the drafts, the clock and the pills; null when one is still there or messaging is down.
   */
  PictureSendQueue* BeginPictureQueue(const std::string& thread_id);
  /** The queue's ports: everything it asks for, for queue `serial` only (a late call for an older one does nothing). */
  PictureSendPorts MakePictureSendPorts(uint64_t serial);
  /** After any call into the queue: the composer follows the slot, and a queue with nothing left to happen goes. */
  void SettlePictureQueue();
  /** What the picture preview page needs from this screen (its queue, files, textures, words). */
  PictureSendHost MakePictureSendHost();
  /** One picture of a several-photo pick arrived from the library (`path` empty: it could not be copied). */
  void OnPhotoBatchItem(uint64_t pick_serial, size_t index, size_t total, std::string path);
  /** The library has handed over every picture of the pick (or none: it was cancelled). */
  void OnPhotoBatchDone(uint64_t pick_serial);
  /** The bottom pill follows the queue: "Sending 3 of 9" while it runs. */
  void ShowPictureProgress();
  /** How the queue ended: the pill's last word, and the metrics line. */
  void OnPictureQueueDone(const PictureSendReport& report);
  // The wait between a pick and its choice / bubble (DEV/PLAN-attachment-bubbles-multi-pick.md step 3): a pick serial
  // per picker opening, the centred overlay while it is prepared, its late results dropped once it is over.
  uint64_t ArmPick();
  void ShowPick(uint64_t serial);
  void EndPick(uint64_t serial);
  /**
   * A late result of pick `serial`: the chat it was armed in when it is current and that chat is still open (the only
   * chat it may go to); otherwise none, and its files are released.
   */
  std::optional<std::string> TakePickResult(uint64_t serial, const std::vector<std::string>& files);
  /** The current pick ends now (lock, sign-out, chat deleted, Cancel): preparing ends too, the entries come back. */
  void AbandonPick();
  /** chat_.picking_busy / picking_busy_slow from the pick and the open chat (PickOverlayOf). */
  void RefreshPickOverlay();
  /** The overlay's Cancel: the pick ends; a video waiting for its poster stops sending. */
  void OnCancelPicking();
  /**
   * The picture being sent shows as a bubble at the end of its chat (`picture` above; DEV/PLAN-picture-upload-bubble.md):
   * an event's result is taken, and the transcript drawn again when the row changed and its chat is open (`redraw`:
   * false when the caller draws it right after, in the same UI task).
   */
  void ApplyPendingUpload(PendingUploadStep step, bool redraw = true);
  /**
   * `event` applied to the bubble `row_id` (each send carries its own row: several bubbles may show while a picture
   * queue runs, its failed ones kept); nothing when that row is gone. Redrawn as ApplyPendingUpload.
   */
  void ApplyPendingUploadTo(const std::string& row_id, const PendingUploadRows::Event& event, bool redraw = true);
  /** chat_.attachment_in_transcript: the open chat shows that bubble, so the composer's upload card / chip stay hidden. */
  void RefreshAttachmentInTranscript();
  /** A voice recording that could not be sent and was not staged: the chip offers Retry / Discard for the file. */
  void KeepVoiceForRetry(const std::string& path, const std::string& thread_id);
  /**
   * A voice send ended with nothing staged: the bubble `row_id` shows Failed with `path` behind its own Retry / ✕; the
   * bubble goes when the recording is empty or gone or it was cancelled. No bubble: the chip's single Retry.
   */
  void FailVoiceKeepingRecording(const std::string& row_id, const std::string& path, const std::string& thread_id);
  /** A failed voice bubble's Retry when it holds its own recording: a fresh job for that file, at the end of the line. */
  void EnqueueVoiceResend(const std::string& row_id, const std::string& thread_id, const std::string& path);
  /**
   * Failed voice bubbles holding their own recording go, rows and files: all of them on the lock (it wipes the
   * recording folder), or `thread_id`'s when that chat is deleted.
   */
  void DropFailedVoiceRecordings(const std::optional<std::string>& thread_id = std::nullopt);
  /** Which way the voice message just sent went (`voice_send_won_`), for its voice.send line. */
  void NoteVoiceSendWon(const ThreadMessage& sent);
  /**
   * A finished recording: its bubble goes up at once and its send joins voice_queue_, which sends the recordings one
   * at a time in the order they were made, behind any other attachment in flight. Never refused for being busy.
   */
  void EnqueueVoiceSend(const std::string& path, const std::string& thread_id);
  /** A failed voice bubble's Retry: the row shows uploading again and its resend joins voice_queue_. */
  void EnqueueVoiceRetry(const std::string& row_id, const std::string& thread_id);
  /** The voice jobs that have not started leave (lock, sign-out, a deleted chat: `thread_id`) and their bubbles are fixed. */
  void DropWaitingVoice(const std::optional<std::string>& thread_id = std::nullopt);
  /** The row of a voice job that left the line before it started: a recording's goes, a retry's is Failed again. */
  void SettleLeftVoiceJob(const VoiceSendJob& job);
  /** Resends the staged copy `record_id` (the Retry of a bubble `item_id`, or of the chip: empty); `on_done` once it ends. */
  void StartAttachmentRetry(const std::string& item_id, const std::string& record_id, const std::string& thread_id,
                            std::function<void(bool sent)> on_done = {});
  /**
   * Retry / discard the failed send: a bubble's carries its item id (that item and its own staged copy); the chip's
   * none (the send it shows). The staged ciphertext is what a retry sends again.
   */
  void OnRetryAttachmentSend(const std::string& item_id = {});
  void OnDiscardAttachmentSend(const std::string& item_id = {});
  /** Cancel the send in flight (a bubble's disc carries its id): its upload stops, its staged copy goes, nothing is sent. */
  void OnCancelAttachmentSend(const std::string& item_id = {});
  /** The upload card for a send starting (`name`: the file's name) or ended (empty): bar, badge, picture, ✕. */
  void ResetUploadCard(const std::string& name);
  /** Show the chip for a staged send of the open thread that did not finish (also after the app was killed). */
  void RefreshPendingAttachmentChip(bool force);
  void ShowFailedAttachment(const std::string& record_id, const std::string& name);
  /** Progress for the chip: the percent line, with a "please stay in PP" hint for a large file. */
  /**
   * `offline_stop`: set when the progress stopped the transfer because the device stayed offline (OfflineHold); the
   * result handler then says "no connection" (the HTTP layer reports a cancelled request).
   */
  std::function<bool(uint64_t done, uint64_t total)> MakeSendProgress(
      std::shared_ptr<std::atomic<bool>> offline_stop = nullptr);
  /** The composer sends to an AI thread: an open AI thread, or Home (its first send opens one). */
  bool InAiComposerContext() const override;
  /** Phones: "Photo / File / Cancel" before a picker (both composers). */
  void ShowAttachChoice(std::function<void()> photo, std::function<void()> file) override;
  /**
   * A thread's transcript window drawn as bound rows, AI question images decorated. Our messages carry their send mark
   * (#210); when a clock shown now turns red on its own, the chat is redrawn then (ArmSendMarkRedraw).
   * `remember_drawn` (the chat view's own build, not a read for the AI): drawn_attachments_ is rebuilt from these rows.
   */
  std::vector<MessageDisplayRow> BuildMessageRows(const std::string& thread_id, std::optional<int64_t> oldest_inclusive,
                                                  std::optional<int64_t> newest_inclusive, bool remember_drawn = false);
  void OpenAttachment(const std::string& message_id);
  /**
   * A tap on an attachment card (open_attachment), as AttachmentExit decides: a computer saves a file (the save
   * dialog), everything else opens as before (OpenAttachment).
   */
  void TapAttachment(const std::string& message_id);
  /** One message's attachment as a draw would describe it, looked up now (UI thread, one read); none: not an attachment. */
  std::optional<DrawnAttachment> LookUpAttachment(const std::string& message_id);
  /** Remembers how `message`'s attachment row is drawn now (`view`), for a later tap or menu (drawn_attachments_). */
  void RememberDrawnAttachment(const ThreadMessage& message, const TranscriptAttachmentView& view, bool voice_bubbles,
                               int voice_max_seconds);
  /**
   * A tap on a picture (or its placeholder): a spinner now, then the system viewer with the full picture (found from
   * the message, never the bubble's preview); its own share / save are the way out.
   */
  void OpenPicture(const std::string& message_id);
  /** Group chats: a tap on a member's avatar or name opens their page over the chat (contact or not). */
  void OpenGroupSender(const std::string& message_id);
  void DownloadAttachment(const std::string& message_id);
  void RetryAttachmentDownload(const std::string& message_id);
  void SyncComposerInputState() override;
  void DeleteMessageForMe(const std::string& message_id) override;
  void DeleteMessageForEveryone(const std::string& message_id) override;
  bool IsDeletedMessage(const std::string& message_id) const override;
  /** What the user reads in a message of the active thread ("" when not found). */
  std::string MessagePlainText(const std::string& message_id) const override;
  bool IsContactCard(const std::string& message_id) const override;
  bool IsCallRecord(const std::string& message_id) const override;
  void AddContactFromCard(const std::string& message_id) override;
  std::optional<DrawnAttachment> DrawnAttachmentOf(const std::string& message_id) const override;
  void SaveAttachmentAs(const std::string& message_id) override;
  void ShareAttachment(const std::string& message_id, ui::Vector2i position) override;
  /**
   * Reply / Ask AI: shows `text` as the quote bar above the composer; the next send carries it as a quote.
   * `draft_prefix` ("@ai " for Ask AI) is put into the input with the caret after it.
   */
  void StartQuoteReply(const std::string& text, const std::string& draft_prefix = {}) override;
  void CancelQuoteReply();
  /** Editing a sent message (DEV/PLAN-message-edit.md): ChatMenusHost's "Edit", and the bar's × while editing. */
  bool CanEditMessage(const std::string& message_id) override;
  void StartEditMessage(const std::string& message_id) override;
  void CancelEditMessage();
  /** Send while editing: unchanged → just ends; too late → a pill, still editing; sent → ends, the draft comes back. */
  void SendEditedMessage();
  /**
   * The edited message was deleted meanwhile (another device): editing ends with a pill (in the open chat). The edit bar
   * then follows the chat shown.
   */
  void EndEditIfTargetGone();
  /** The edit bar and the attach / voice lock follow the open chat's edit state. */
  void SyncEditingState();
  /** A tap on the quote of reply `reply_message_id`: scrolls to the message it quotes, if it is on screen. */
  void JumpToQuotedMessage(const std::string& reply_message_id);
  /** A tap on link `link_index` of a plain-text message: confirms the host, then opens it. */
  void OpenMessageLink(const std::string& message_id, int link_index);
  void SyncDisplayFromThread() override;
  /** Last time a display sync ran the whole-thread attachment backfill. */
  std::chrono::steady_clock::time_point last_attachment_backfill_{};
  std::chrono::steady_clock::time_point last_call_joinable_poll_{};
  /** Each second: whether the open group chat has a call this device may join (calls change it). */
  void RefreshCallJoinable(std::chrono::steady_clock::time_point now);
  void SetCallBarText(const std::string& text);
  /** The call bar's "Join": joins the group's call, or says it has ended when it is gone. */
  void OnJoinGroupCall();
  void OpenChatLink(const std::string& entry_id, int link_index) {
    OpenChatLinkIn(ActiveThreadId(), entry_id, link_index);
  }
  /**
   * Link `link_index` of answer `entry_id` in `thread_id` (after a restart rebuilt from its stored text). A news item
   * opens the news reader when `news_reader` (the chat page); otherwise, like any link, the browser.
   */
  void OpenChatLinkIn(const std::string& thread_id, const std::string& entry_id, int link_index,
                      bool news_reader = true);
  /** The link table of answer `entry_id` (rebuilt from its stored text after a restart); null when there is none. */
  const std::vector<MarkdownLink>* ChatLinksOf(const std::string& thread_id, const std::string& entry_id);

  // --- the news reader (DEV/PLAN-news-inline-digest.md; view "news_reader") -----------------------------------
  /** A tap on news item `tapped` of an answer whose links are `links`, in AI thread `thread_id`. */
  void OpenNewsReader(const std::string& thread_id, const std::vector<MarkdownLink>& links, const MarkdownLink& tapped);
  /** Shows item `index` of the open reader's list: its stored digest, or reads it (NewsReader decides). */
  void ShowNewsItem(size_t index);
  /** Previous / Next under the reader (`step` -1 / +1); ignored while it is off. */
  void StepNewsReader(int step);
  /** "Open original": the item's own URL in the system browser (the trusted path). */
  void OpenNewsOriginal();
  /** Closes the reader sheet if it is open (lock, sign-out, the chat deleted or cleared). */
  void CloseNewsReader();
  /** The reader's state into the data model. */
  void ApplyNewsReader();
  /** The news items `thread_id` has read (read once, dropped when a read ends or the chat is cleared). */
  const NewsReads& NewsReadsOf(const std::string& thread_id);
  /** Greys the news items already read in `rows` of `thread_id`; `news_texts`: message id -> stored text of the
   *  answers drawn with news items. */
  void MarkReadNewsItems(std::vector<MessageDisplayRow>& rows, const std::string& thread_id,
                         const std::map<std::string, std::string>& news_texts);
  /** A news read of `thread_id` ended (digest, failure, error or stop): the reader and the list show what it left. */
  void OnNewsReadEnded(const std::string& thread_id);
  /** A link someone sent in a chat: opens an https URL after the user confirms its host; the dialog offers to stop
   *  asking until the app restarts. */
  void ConfirmAndOpenUrl(const std::string& url);
  /** A link from our own feed (Home's briefs, an AI answer's article or source): opens an https URL with no prompt
   *  (Kenneth 2026-11-04). The caller says where it came from; nothing is matched on the URL itself. */
  void OpenTrustedUrl(const std::string& url);
  /** Whether message `entry_id` of `thread_id` was written by the AI assistant (false when it cannot be read). */
  bool MessageFromAssistant(const std::string& thread_id, const std::string& entry_id);
  void RestoreWorkingSetsFromActiveThread();
  void SyncShellSessions() override;
  void UpdateThreadChrome();
  void ResetChatPanelState();
  std::string failed_record_id_;
  /** The picture being sent, as a temporary bubble at the end of its chat; one at a time. */
  PendingUploadRows pending_uploads_;
  uint64_t pending_upload_serial_ = 0;
  /** The pictures of one pick being chosen or sent one at a time (none most of the time); see BeginPictureQueue. */
  std::unique_ptr<PictureSendQueue> picture_queue_;
  /** Voice messages waiting for the attachment slot (always there; empty most of the time). */
  std::unique_ptr<VoiceSendQueue> voice_queue_;
  /** Names the current queue: an item's late end or a late tick for an older one is told apart. */
  uint64_t picture_queue_serial_ = 0;
  /** The preview page of the pictures picked together (hosted here like OpenHomeEdit's layer). */
  PictureSendController picture_send_;
  /** The several-photo pick being copied out of the library (iPhone / iPad); none otherwise. */
  struct PhotoBatch {
    uint64_t pick_serial = 0;
    std::string thread_id;
    uint64_t session = 0;
    /** The queue made for it (0: none yet). */
    uint64_t queue_serial = 0;
    size_t seen = 0;
  };
  /** When the system picker finished (the preview page's first picture is timed from here); 0: not timing. */
  int64_t picture_picker_done_ms_ = 0;
  std::optional<PhotoBatch> photo_batch_;
  /** The progress pill's last text, and the time spent waiting out the send pace (the batch metric). */
  std::string picture_progress_text_;
  bool picture_paced_ = false;
  int64_t picture_pace_since_ms_ = 0;
  int64_t picture_wait_ms_ = 0;
  AttachmentPicks picks_;
  /**
   * A bubble's disc ignores a cancel tap this long after the tap that started or retried it: the retry disc and the
   * cancel disc sit in the same place, and a tap delivered twice (touch and the mouse made from it) must not cancel.
   */
  static constexpr std::chrono::milliseconds kBubbleTapGuard{600};
  std::chrono::steady_clock::time_point bubble_tap_guard_until_{};
  /** The chat whose video bubble waits for its poster (the card stays hidden there); empty when none. */
  std::string video_bubble_thread_;
  /** Set by Cancel on the chip while a send is in flight; a new one for each send. */
  std::shared_ptr<std::atomic<bool>> send_cancel_;
  /** A voice recording waiting for Retry / Discard (nothing was staged), and the chat it is for. */
  std::string failed_voice_path_;
  /** "direct" or "server": the last voice send's winning leg, read once by the voice.send line. */
  std::string voice_send_won_;
  std::string failed_voice_thread_;
  std::chrono::steady_clock::time_point last_voice_call_check_{};
  /** Where the attach menu ends: just above the attach button (B90), refreshed on each tap. */
  /** Deletes a picked / captured file once it is read or given up; tests swap it to count the releases. */
  std::function<void(const std::string& path)> release_pick_ = [](const std::string& path) { ReleasePickedFile(path); };
  AttachmentPreparing preparing_;
  /** Held while the system copies a picked / captured video, so the copy gets background time on iOS. */
  std::unique_ptr<BackgroundActivity> preparing_activity_;
  int attach_menu_x_ = 12;
  int attach_menu_y_ = 120;
  /** Attachments being opened right now (an Open decrypts on a worker; a second tap waits for the first). */
  std::unordered_set<std::string> opening_message_ids_;
  /**
   * The open chat's attachment rows as BuildMessageRows last drew them (kind, downloaded, the name to offer): a tap or
   * a menu decides with them on the UI thread, nothing decrypted or read from the store. Emptied for another chat.
   */
  std::unordered_map<std::string, DrawnAttachment> drawn_attachments_;
  std::string drawn_attachments_thread_;
  /** Desktop "Save as" (one at a time): its own state, apart from opening_message_ids_ (neither stops the other). */
  std::unique_ptr<AttachmentSaveFlow> save_flow_;
  /** Phones' "Share…" of a received attachment (the system share sheet); nothing is deleted after it. */
  std::unique_ptr<AttachmentShareFlow> share_flow_;
  /** The system share sheet for a file (PlatformShareFile by default, set in the constructor); tests may swap it. */
  std::function<bool(const std::string& path, const std::string& mime, ShareAnchor anchor)> share_file_;
  PhotoPreviewTracker photo_preview_;
  std::string pending_chip_thread_;
  /** The chat the attachment in flight (preparing, uploading, or the failed one) belongs to. */
  std::string attachment_thread_;
  void UpdateSidebarPreview(const std::string& preview_text) override;
  void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload,
                         const std::string& entry_id) override {
    HandleLocalActionIn(ActiveThreadId(), message, payload, entry_id);
  }
  /** A button of a stored answer of `thread_id` (the open chat, or the ask-AI panel's AI chat under it). */
  void HandleLocalActionIn(const std::string& thread_id, const std::string& message,
                           const std::optional<std::string>& payload, const std::string& entry_id);
  QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend& item) override;
  void PutBackDraft(const std::string& thread_id, const std::string& text) override;
  /**
   * Text that did not go out goes back to `key`'s composer (a thread id, or ThreadDrafts::kHome), merged with what it
   * holds as `mode` says (MergeDraftText); a conversation not on screen keeps it in drafts_, never the one shown.
   */
  void PutTextBack(const std::string& key, const std::string& text, DraftPutBack mode);
  /** The draft key of what the composer shows now: Home, or the open thread ("" = none shown). */
  std::string CurrentDraftKey() const;
  /** An AI send while every turn is taken: confirm "@ai+" sharing first, then queue it (B112). */
  void QueueAgentSend(const std::string& thread_id, const std::string& text, std::optional<std::string> payload,
                      bool thread_created_for_this, bool already_confirmed = false);
  /** The "share with the AI?" dialog for an "@ai+" / "@ai++" send; "don't ask again" is remembered. */
  void ConfirmShareWithAi(const std::string& thread_id, const std::string& text, std::function<void(bool ok)> then);
  /** The dialog itself (also the router's confirm hook): the one copy of its wording. */
  void ShowShareWithAiDialog(const std::string& thread_id, AtAiMode mode,
                             std::function<void(bool ok, bool dont_ask)> done);
  /** A send that did not go out: its text back in the composer, if that thread is open and the composer empty. */
  void RestoreDraftIfEmpty(const std::string& thread_id, const std::string& text);
  /** The open thread's queued send is cancelled: its text goes back to the composer. */
  void CancelQueuedHere();
  /** Same decision SendUserText makes, before the composer is cleared: false = keep the draft. */
  bool ComposerSendAllowed(const std::string& text);
  /** The composer's send of a group text with mentions (`reply`: the typed text; `out`: with any quote). */
  void SendWithMentions(const std::string& thread_id, const std::string& reply, const std::string& out,
                        std::vector<TextSpan> spans);
  /** Enter / Tab choose the `@` picker's first line while it is open over the composer. */
  void RegisterMentionPickerKeys();
  void RefreshFromMessaging();
  void OnLoadOlderHistory();
  void OnRetryPeerDial();
  void OnMessagesScroll();
  void OnSessionsScroll();
  void OnSessionsLoadMore();
  void FillSessionListAfterLayout();
  void SyncComposerOverlay();
  ui::Element* SessionListElement() const;
  /** The AI conversation list on screen: the phone's drawer, else the desktop's AI page list; null when neither shows. */
  ui::Element* AiSessionListElement() const;
  void OnAiSessionsScroll();
  void OnJumpToLatest();
  // Search inside the open chat (DEV/PLAN-chat-search.md). The decisions are ThreadSearchSession's; these bind
  // them to the screen, build the index on a worker and scroll to the hits.
  void OnOpenSearch() override;
  /** The one way the search ends (Cancel, Esc, another chat, background, profile reset): the index is dropped.
   *  `refocus_composer`: the user closed it, so typing goes on in the composer. */
  void CloseSearch(bool refocus_composer = false);
  void OnSearchQueryChanged();
  /** Enter in the search box: searches what was just typed, else steps to the next older hit. */
  void OnSearchEnter();
  void OnSearchOlder();
  void OnSearchNewer();
  /** Copies the session's view into the data model and marks the shown rows again (no rows rebuilt). */
  void ApplySearchView();
  /** Scrolls to the session's pending jump; a hit whose message is gone is dropped and the next one taken. */
  void JumpToSearchHit();
  /** Reads and decrypts the chat on a worker, indexes it there; the UI thread gets the index for `generation`. */
  void StartSearchIndex(uint64_t generation);
  /** Messages of the searched chat were added or removed: rebuild the index (at most every 2 s, one at a time). */
  void OnSearchMessagesChanged();
  void OnAgentTurnEnded(const std::string& thread_id) override;
  bool AskAiPanelShows(const std::string& thread_id) const override { return ask_ai_.Shows(thread_id); }
  // The ask-AI panel's side of the screen (AskAiPanelHost).
  bool AskAiCompact() const override;
  void AskAiOpenEmoji(std::function<void(const std::string& emoji)> insert) override;
  std::string AskAiSharedThread(bool create) override;
  std::string AskAiNewThread() override;
  std::vector<AskAiRow> AskAiRecentRows(const std::string& thread_id, size_t limit) override;
  bool AskAiLastIsQuestion(const std::string& thread_id) override;
  QueuedSendOutcome AskAiSendNow(const QueuedAgentSend& item) override { return RunQueuedAgentSend(item); }
  std::string AskAiStoreQuestion(const std::string& thread_id, const std::string& text) override;
  int AskAiPushSheet(const PaneSpec& spec, std::function<void()> on_closed) override;
  void AskAiCloseSheet(int layer_id) override;
  void AskAiOpenInChat(const std::string& thread_id) override;
  void AskAiLater(std::function<void()> fn) override;
  void AskAiRunAction(const std::string& thread_id, const std::string& entry_id, int action_index) override;
  void AskAiOpenLink(const std::string& thread_id, const std::string& entry_id, int link_index) override {
    // The panel holds one sheet at a time: a news item there opens the browser, as before.
    OpenChatLinkIn(thread_id, entry_id, link_index, /*news_reader=*/false);
  }
  bool AskAiIntroSeen() const override;
  void AskAiMarkIntroSeen() override;
  void AskAiToast(const std::string& text) override { ShowToast(text); }
  bool AskAiMessagingReady() const override { return messaging_ready_ && facade_; }
  Roe<std::string> AskAiEnsureSupport() override;
  Roe<void> AskAiSendToSupport(const std::string& thread_id, const std::string& text) override;
  std::vector<SupportMessage> AskAiSupportMessages(const std::string& thread_id, size_t limit) override;
  void AskAiSyncInbox() override;
  bool AskAiAttentive() const override;
  void AskAiMarkRead(const std::string& thread_id) override;
  /** ChatMenusHost: "⋯ → Ask AI" (phones, chats with people). */
  bool AskAiPanelAvailable() const override { return AskAiCompact(); }
  void OpenAskAiPanel() override { (void)ask_ai_.Open(AskAiEntry::ChatMenu); }
  /** Opens the thread in the full chat page (Sessions tab, chat pane, the phone's chat page). */
  void ShowThreadInChat(const std::string& thread_id);
  /** The chat title: the group info sheet (a group chat on a phone), else the small peer menu. */
  void OpenPeerSheet(ui::Event& ev);
  // The group info sheet's side of the screen (GroupInfoSheetHost).
  bool GroupInfoCompact() const override { return AskAiCompact(); }
  GroupInfoScreen GroupInfoRead(const std::string& thread_id) override;
  int GroupInfoPushSheet(const PaneSpec& spec) override;
  void GroupInfoCloseSheet(int layer_id) override;
  bool GroupInfoSheetOpen(int layer_id) const override;
  void GroupInfoCall(const std::string& thread_id, bool video) override;
  void GroupInfoMute(const std::string& thread_id, bool muted) override;
  void GroupInfoRename(const std::string& thread_id, bool for_everyone) override;
  void GroupInfoRestoreName(const std::string& thread_id) override;
  void GroupInfoViewAll(const std::string& thread_id) override;
  void GroupInfoAddMember(const std::string& thread_id) override;
  void GroupInfoChoosePreset(const std::string& group_id, GroupPreset current) override;
  /** G017, the owner: Everyone (Open) / Admins only (Managed), as a menu at `position` (an action sheet on phones). */
  void ChooseGroupPreset(const std::string& group_id, GroupPreset current, ui::Vector2i position) override;
  void GroupInfoTapMember(const std::string& thread_id, const std::string& identity) override;
  void GroupInfoLeave(const std::string& thread_id) override;
  void GroupInfoDissolve(const std::string& group_id) override;
  void GroupInfoInviteLink(const std::string& group_id) override;
  void GroupInfoRevokeLink(const std::string& group_id, const std::string& link_id) override;
  /** G021: the invite link sheet's and the join prompt's ports. */
  GroupLinkSheetPorts MakeGroupLinkPorts();
  GroupJoinPromptPorts MakeJoinPromptPorts();
  /** Ask, then revoke the link (off the UI thread); `done(true)` once revoked. */
  void RevokeLinkAfterConfirm(const std::string& group_id, const std::string& link_id, std::function<void(bool)> done);
  // The owner's "choose a new owner" sheet (GroupLeaveSheetHost).
  OwnerLeaveSheet GroupLeaveRead(const std::string& thread_id) override {
    return presenter_.OwnerLeaveSheetOf(thread_id);
  }
  int GroupLeavePushSheet(const PaneSpec& spec) override { return GroupInfoPushSheet(spec); }
  void GroupLeaveCloseSheet(int layer_id) override { GroupInfoCloseSheet(layer_id); }
  bool GroupLeaveSheetOpen(int layer_id) const override { return GroupInfoSheetOpen(layer_id); }
  void GroupLeaveConfirm(const std::string& message, std::function<void(bool ok)> done) override;
  bool GroupLeaveStillPossible(const std::string& thread_id, const std::string& identity) override {
    return presenter_.CanHandOverTo(thread_id, identity);
  }
  void GroupLeaveHandOver(const std::string& thread_id, const std::string& group_id,
                          const std::string& identity) override;
  void GroupLeaveDissolve(const std::string& group_id) override { DissolveGroupAfterConfirm(group_id); }
  void GroupLeaveToast(const std::string& text) override { ShowToast(text); }
  void WireMessagingBindings();
  void WithSecrets(std::function<void()> action);

  bool MessagingInitialized() const override;
  bool MessagingReady() const override;
  const std::string& ActiveThreadId() const override;
  /** Marks the open chat read (and clears its notification) when ShouldMarkOpenChatRead() says so. */
  bool MarkOpenChatReadIfLooking();

  ShellChromeSnapshot ChromeSnapshot() const override;
  ChatSurfaceSnapshot BuildSurfaceSnapshot() const;
  void NotifySurfaceChanged() override;
  /** The call site is passed on as the request's reason (ShellHost::RequestSyncLayout). */
  void ShellSyncLayout(bool restore_focus_after = false, std::source_location where = std::source_location::current());
  void ShellSelectNavTab(NavTab tab);
  void ShellSetPrimaryPane(const std::string& key);
  void ShellOpenCompactChat();
  void ShellCloseCompactChat();
  void ShellSetActivity(bool visible, const ui::String& message = {}) override;
  void ShellRemountNavRail();
  void ShowToast(const std::string& message, ToastDuration duration = ToastDuration::Short) override;
  void ShowConfirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result,
                   bool destructive = false) override;
  void ShowConfirmWithCheckbox(const std::string& title, const std::string& message, const std::string& checkbox_label,
                               bool checkbox_default, std::function<void(bool, bool)> on_result,
                               const std::string& ok_label = {});
  void ShowPrompt(const std::string& title, const std::string& message, const std::string& default_value,
                  std::function<void(bool, std::string)> on_result) override;

  ui::Context* context_ = nullptr;
  ConversationsFacade* facade_ = nullptr;
  /** The chats list's decisions and buttons (pp-app-probe drives the same presenter). */
  ChatPresenter presenter_;
  /** When each muted group last alerted on a mention (G015: at most once a minute). */
  std::unordered_map<std::string, int64_t> muted_mention_alert_ms_;
  std::function<void(ToolRegistry&)> register_messaging_tools_;
  ContactsNotifyPorts contacts_notify_;
  PeoplePickerNotifyPorts people_picker_notify_;
  EmojiPickerNotifyPorts emoji_picker_notify_;
  ShellSetupPorts shell_setup_;
  SessionStore* session_store_ = nullptr;
  BadgeNotifyPorts badge_notify_;
  /** MarkOpenChatReadIfLooking() is running: whatever MarkRead stirs up cannot start it again. */
  bool reading_open_chat_ = false;
  InputCoordinator* input_ = nullptr;
  CallActionsPorts call_actions_;
  UnlockEnsurePorts unlock_ensure_;
  ShellNavigationPorts shell_navigation_;
  ShellFeedbackPorts shell_feedback_;
  ChatSurfaceNotifyPorts surface_notify_;
  MessagingUiPorts messaging_ui_;
  ChatState chat_;
  ShellState shell_;
  bool messaging_ready_ = false;
  /** Whether this device holds a recovery phrase: one vault read per messaging session / identity change. */
  std::optional<bool> recovery_phrase_present_;
  UpdateCardPorts update_card_;
  DeviceRemovedNotice device_removed_{[this] { OnDeviceRemovedNoticeChanged(); }};
  void OnDeviceRemovedNoticeChanged();
  /** machine.json's memory of "Later", read when the announced version changes (not on every list refresh). */
  UpdateCardState update_card_state_;
  std::optional<std::string> update_card_state_for_;
  /**
   * One up per WireMessagingBindings (and when messaging goes away): a messaging-ready task's result is stored only
   * when the session it started in is still this one (MessagingReadyTasks).
   */
  uint64_t messaging_session_ = 0;
  /** Push binding, registration renewal and the Brief key mint off the UI thread. Made in BindConversationsFacade. */
  std::unique_ptr<MessagingReadyTasks> ready_tasks_;
  void MakeReadyTasks();
  /** B216: the relay does not know this account; register again, or ask the user (RegistrationRecovery). */
  RegistrationRecovery recovery_;
  std::function<void(std::function<void(bool ok)> done)> reconnect_account_;
  std::function<bool(const std::string&, std::function<void(const ContactRefreshResult&)>)> refresh_contact_;
  /** The choice made on a failed message's red mark: update their card or ours, then send it again (in memory only). */
  SendFailureFix send_fix_;
  void MakeSendFixPorts();
  void OnRelayRegistrationSignal(RelayRegistrationSignal signal);
  /** A renewal ended (MessagingReadyTasks' renew_hint): tells the recovery how. */
  void OnRecoveryRenewal(const RenewResult& result, bool auto_renew);
  void ApplyRecovery(const RecoveryActions& actions);
  /** What each recovery action does here (the renewal: mark the registration expired, stored, then run the task). */
  RecoveryHooks MakeRecoveryHooks();
  void OnReconnectTapped();
  /** After messaging went away: the recovery starts over (the row goes). */
  void ResetRecovery();
  /** Its AppLifecycle background listener (removed at Shutdown). */
  uint64_t lifecycle_background_id_ = 0;
  /** Its AppLifecycle foreground listener: the send marks are worked out again (a clock may be over). */
  uint64_t lifecycle_foreground_id_ = 0;
  /**
   * #210: one coordinator timer redraws the open chat when its soonest clock turns red (`send_mark_redraw_at_`, Unix
   * ms; 0: none armed). `send_mark_redraw_turn_` tells a stale firing from the armed one; `send_mark_alive_` keeps a
   * firing from reaching a destroyed controller.
   */
  uint64_t send_mark_timer_ = 0;
  int64_t send_mark_redraw_at_ = 0;
  uint64_t send_mark_redraw_turn_ = 0;
  std::shared_ptr<int> send_mark_alive_ = std::make_shared<int>(0);
  void ArmSendMarkRedraw(std::optional<int64_t> at_ms);
  /** A message sent again by a tap is drawn red again: the pill says why, once (ChatPresenter::ResentTurnedRed). */
  void TellResentTurnedRed(const std::string& thread_id, const std::vector<std::string>& red_ids);
  /** The open chat's rows again (their send marks recomputed), nothing else refreshed. */
  void RedrawSendMarks();
  /**
   * A tap on a message's red mark (ChatPresenter::TapSendMark): our message sent again as itself once its automatic
   * sending has stopped (B236), else the bottom pill says why (still trying, or why it did not go out).
   */
  void OnSendMarkTapped(const std::string& message_id);
  /** What a tap (or a resend after a card update) came to, told as the composer says it. */
  void ApplySendMarkTap(const std::string& thread_id, const std::string& message_id, const SendMarkTap& tap);
  /**
   * B236: the chat `thread_id` is held (the recipient has not added us; ChatPresenter::SendHeld): the pill says so, every
   * time, and true — the caller sends nothing (the typed text stays). False: send as usual.
   */
  bool RefuseSendWhileHeld(const std::string& thread_id);
  /** A message sent again as itself after a tap (ConversationsFacade::ResendMessageAsync). */
  void ResendSameMessage(const std::string& thread_id, const std::string& message_id);
  /**
   * Step 0 of DEV/PLAN-chat-picture-previews.md, measurement only. Opening a chat (FinalizeThreadDisplay) arms
   * `armed`; the next display sync records its rows, pictures and build time and sets `layout_pending`; the next
   * ShellHost::SyncLayout to end (OnSyncLayoutMeasured) adds its own time and emits chat.open.
   */
  struct ChatOpenProbe {
    bool armed = false;
    bool layout_pending = false;
    int64_t rows = 0;
    int64_t pictures = 0;
    int64_t placeholders = 0;
    int64_t build_ms = 0;
    /** The open's first screen (DEV/PLAN-chat-first-screen-rows.md): ok / short / miss. */
    ChatTranscriptScroller::FirstScreen first_screen = ChatTranscriptScroller::FirstScreen::Miss;
    std::chrono::steady_clock::time_point built_at{};
  };
  ChatOpenProbe open_probe_;
  /** The chat page was on screen at the last Update; leaving it (Back, another tab, no thread) starts the 5 s wait. */
  bool chat_showing_ = false;
  /** `*_turn` tells a stale timer firing from the armed one; `chat_window_alive_` keeps one from a destroyed controller. */
  uint64_t chat_exit_turn_ = 0;
  uint64_t chat_paged_turn_ = 0;
  std::shared_ptr<int> chat_window_alive_ = std::make_shared<int>(0);
  bool ChatShowing() const;
  void PollChatShowing();
  /**
   * B203: the chat pictures the page shows, counted per file. A file no row shows any more (trimmed, deleted, another
   * chat; the chat left, the vault locked) is queued, and its texture is released (ui::ReleaseTexture) two frames
   * later, once the page no longer has it.
   */
  ChatPictureTextures picture_textures_;
  std::vector<std::string> textures_release_queued_;
  std::vector<std::string> textures_release_ready_;
  void SyncPictureTextures();
  void QueueTextureRelease(std::vector<std::string> srcs);
  void ReleaseQueuedTextures();
  /** The vault was seen locked (the chat's pictures were turned into placeholders); unlocked, the rows are redrawn. */
  bool vault_locked_seen_ = false;
  /** B203: previews arriving are drawn into their rows at most every 250 ms, only the picture clips replaced. */
  PreviewRedrawBatcher preview_batcher_;
  std::unordered_set<std::string> previews_failed_;
  void OnChatPreviewDone(const std::string& thread_id, const std::vector<uint8_t>& content_hash, bool made);
  void RedrawPreviews();
  /** chat.previews: the open's placeholders until their previews are drawn (or the chat is left). */
  ChatPreviewsProbe previews_probe_;
  void FinishPreviewsProbe();
  /** Once the open's layout has settled: which placeholders are on screen (chat.previews' visible_ms). */
  void MeasureVisiblePreviews();
  void ScheduleChatWindowMetric(std::chrono::milliseconds delay, bool exited);
  bool mesh_ready_ = false;
  ChatTranscriptScroller scroller_;
  WorkingSetController working_set_;
  ChatThreadChrome chrome_;
  ChatWidgetHost widgets_;
  std::optional<PendingReply> pending_reply_;

  /** entry id -> https links its rendered bubble refers to (open_chat_link index). */
  std::map<std::string, std::vector<MarkdownLink>> chat_links_;
  /** The agent's side of the screen (turns, streaming, held sends, AI images, its widgets). */
  AgentTurnController agent_{*this, chat_, chat_links_, widgets_, working_set_, shell_feedback_};
  /** The ask-AI half sheet over any page (DEV/PLAN-ui-batch5-ask-ai.md 5a). */
  AskAiPanel ask_ai_{*this, agent_};
  std::function<Roe<std::string>()> ensure_support_;
  std::function<void(const std::string& section_id)> open_settings_section_;
  FlowCoordinatorPorts flow_coordinator_;
  /** The screen's menus (long press / right-click, header buttons, pickers' fallbacks). */
  ChatMenus menus_{*this,           presenter_,       chat_,
                   working_set_,    contacts_notify_, emoji_picker_notify_,
                   shell_feedback_, call_actions_,    people_picker_notify_};
  ChatPeerSheet peer_sheet_{*this, presenter_, chat_, contacts_notify_, people_picker_notify_};
  /** The phone's group info sheet over a group chat (DEV/PLAN-ui-batch8-group-pages.md 1). */
  GroupInfoSheet group_info_{*this};
  /** The owner leaving a group on a phone: choose a new owner (DEV/PLAN-ui-batch8-group-pages.md 3). */
  GroupLeaveSheet group_leave_{*this};
  /** G021: the "Invite link…" sheet over group info, and joining by a link that arrives. Made in the constructor. */
  std::unique_ptr<GroupLinkSheet> group_link_;
  std::unique_ptr<GroupJoinPrompt> join_prompt_;
  /** The composer's `@` picker in a group chat, and the mentions it recorded per chat (G015). */
  ChatMentionPicker mentions_{*this, presenter_, chat_, context_};
  /** The composer's microphone, and the voice bubbles' player. Made in the constructor (they need `this`). */
  std::unique_ptr<VoiceComposer> voice_;
  std::unique_ptr<VoicePlayback> voice_playback_;
  /** The chat the voice parts last saw: another one cancels a recording and stops a clip. */
  std::string voice_thread_;
  /** A chat was just opened: put the composer back in the remembered mode once the chat can take a voice message. */
  bool restore_voice_mode_pending_ = false;
  // AgentTurnHost, ChatMenusHost
  ConversationsFacade* Facade() const override { return facade_; }
  bool MessagingIsReady() const override { return messaging_ready_; }
  void MintBriefGuestKey(const std::string& llm_base_url) override {
    if (ready_tasks_) {
      ready_tasks_->MintBriefKey(llm_base_url);
    }
  }
  void RegisterMessagingTools(ToolRegistry& tools) override {
    if (register_messaging_tools_) {
      register_messaging_tools_(tools);
    }
  }
  bool skip_link_confirm_this_run_ = false; // set from the confirm dialog's checkbox; never persisted
  // Phone Today page (DESIGN-pp-ui 8.16): the last unread summary and the Today's briefs cache.
  HomeTodaySummary home_summary_;
  std::vector<HomeFavorite> home_favorites_;
  /** The newest calls as last read (RefreshHomeCalls), and whether a read is running. */
  std::vector<call_queries::RecentCall> home_calls_;
  bool home_calls_reading_ = false;
  bool home_calls_again_ = false; // asked again while a read ran: read once more when it ends
  /** The order being dragged in Edit Home (saved when the handle is let go); none while no handle is held. */
  std::optional<HomeWidgetLayout> home_edit_drag_;
  std::string home_edit_drag_id_;
  /** The card the held row is in (for the settle on release), and where on the row the finger took it (px from its
   *  centre; set on the first move, home_edit_grabbed_). */
  ui::ObserverPtr<ui::Element> home_edit_card_;
  float home_edit_grab_px_ = 0.f;
  bool home_edit_grabbed_ = false;
  HomeBriefCache home_brief_;
  std::string home_brief_dir_; // the profile directory the cache was read from
  bool home_brief_fetching_ = false;
  int64_t home_brief_failed_ms_ = 0; // the last fetch that failed; 0 none
  int reactors_layer_ = -1;          // the reactions sheet while it is open
  std::string reactors_message_id_;  // the message it lists
  /** The news reader while it is open: its chat, the list it walks (one item for a news pick) and where it is. */
  struct NewsReaderOpen {
    std::string thread_id;
    std::vector<NewsItem> items;
    size_t index = 0;
  };
  std::optional<NewsReaderOpen> news_reader_;
  int news_reader_layer_ = -1;
  std::map<std::string, std::string> news_reading_url_; // AI thread -> the URL its running news read reads
  std::map<std::string, NewsReads> news_reads_;         // AI thread -> its read items (NewsReadsOf)
  int home_edit_layer_ = -1;                            // the Edit Home sheet while it is open
  bool local_action_in_flight_ = false; // a row action (Add / Message) is waiting on its directory lookup
  bool focus_draft_after_sync_ = false;
  std::optional<int64_t> last_escape_ms_;          // a second Escape within kDoubleEscapeWindowMs stops the turn
  ThreadSearchSession search_;                     // search inside the open chat; its index lives only while open
  std::string search_thread_;                      // the chat it was opened on
  std::string search_thread_tail_;                 // that chat's newest message id when last checked
  std::unordered_set<std::string> search_hit_ids_; // the rows whose matching text the decorator marks
  std::string search_current_id_;
  std::string search_folded_query_;
  /** Typing settles for kSearchDebounce before the query is applied. */
  std::optional<std::chrono::steady_clock::time_point> search_query_due_;
  enum class SearchFocus { None, Input, Composer };
  SearchFocus search_focus_ = SearchFocus::None; // moved after the next layout (data-if shows the element first)
  SessionListWindow session_window_;             // how many rows of the chats list the sidebar draws
  SessionListWindow ai_session_window_;       // how many AI conversations the drawer (or the desktop's AI list) draws
  ThreadDrafts drafts_;                       // unsent text per conversation; the composer widget is shared
  std::map<std::string, std::string> quotes_; // a quote being replied to, per conversation, while another is shown
  void StashComposerExtras(const std::string& key);
  void RestoreComposerExtras(const std::string& key);
  void ForgetComposerExtras(const std::string& key);
};

} // namespace pbr
