#pragma once

#include "feature/ai/AgentSession.h"
#include "feature/presenters/SupportAsk.h"
#include "gui/chat/AgentTurnController.h"
#include "gui/chat/AgentBusyRules.h"
#include "lib/ui/ShellTypes.h"

#include "lib/ui/ContextMenuHost.h"

#include <ui/base/Types.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
} // namespace ui

namespace pbr {

/** How the panel picks, prepares and sends a picture; the defaults are the agent controller's (the chat's path). */
struct AskAiImagePorts {
  std::function<void(std::function<void(std::string path)>)> pick;
  std::function<void(std::function<void(std::string path)>)> capture;
  /** The plus card's Photos (`files_app` false) and File. */
  std::function<void(bool files_app, std::function<void(std::string path)>)> pick_from;
  std::function<void(std::string path, std::function<void(Roe<AgentTurnController::PendingAiImage>)>)> prepare;
  std::function<AgentTurnController::ImageSend(const std::string& thread_id, const std::string& text,
                                               AgentTurnController::PendingAiImage image)>
      send;
  std::function<void(const AgentTurnController::PendingAiImage&)> drop;
};

/** Where the ask-AI panel was opened from (DEV/PLAN-ui-batch5-ask-ai.md 5a). */
enum class AskAiEntry {
  HomeChip,    // a Home chip: its question is asked at once
  ChatMenu,    // a chat's "⋯ → Ask AI": nothing of the chat goes along
  SettingHelp, // a setting's "?": the PP 客服 chat (AskAiPanel::OpenSupport)
  Dot,         // the floating AI dot (5c): like the chat menu, at the sheet's usual height
};

/** One bubble in the panel, drawn like the chat's. */
struct AskAiRow {
  ui::String id;
  bool user = false;
  ui::String content_rml;
};

/** What the panel binds ("ask_ai" data model). */
struct AskAiView {
  /** The PP 客服 chat shows (a settings "?", DEV/PLAN-help-to-support-panel.md): no AI anywhere in the sheet. */
  bool support = false;
  /** PP 客服 mode: the question waits for its reply ("replying…"), or has waited 90 s ("got it, it comes here"). */
  bool support_waiting = false;
  bool support_slow = false;
  /** PP 客服 mode: why nothing can be sent yet (getting ready, not ready, unavailable); "" when it can. */
  ui::String support_note;
  /** The first-time privacy card, until "Got it". */
  bool show_intro = false;
  std::vector<AskAiRow> rows;
  bool thinking = false;
  /** The question waiting for a free turn ("" when none). */
  ui::String queued_text;
  /** The AI could not answer: one plain sentence, and the "Ask PP Support" button. */
  bool error = false;
  bool show_support = false;
  /** The unsent text of the mode shown; kept when the panel closes. */
  ui::String draft;
  /** Camera and attachment buttons (the shared AI conversation only). */
  bool show_media = true;
  /** The picture waiting for its question (being prepared, or ready with its thumbnail). */
  bool has_picture = false;
  bool picture_preparing = false;
  ui::String picture_src;
};

/** What the panel needs from the chat screen (ChatController). UI thread. */
class AskAiPanelHost {
public:
  virtual ~AskAiPanelHost() = default;
  /** The panel is a phone surface; the desktop keeps its own entries. */
  virtual bool AskAiCompact() const = 0;
  /**
   * The shared AI conversation (the one Home and the panel continue): the AI chat used last, so a new one from New
   * chat's "AI chat" is it from then on. `create`:
   * make one, not opened, when there is none. "" when there is none (or messaging is not up).
   */
  virtual std::string AskAiSharedThread(bool create) = 0;
  /** Makes a brand-new AI chat, not opened (the floating dot's panel starts one). "" when it could not. */
  virtual std::string AskAiNewThread() = 0;
  /** The thread's last `limit` messages as chat bubbles, oldest first. */
  virtual std::vector<AskAiRow> AskAiRecentRows(const std::string& thread_id, size_t limit) = 0;
  /** The thread's newest message is the user's (the answer to it never came). */
  virtual bool AskAiLastIsQuestion(const std::string& thread_id) = 0;
  /** Hands the question to the thread's AI now: the router stores it and starts the turn. */
  virtual QueuedSendOutcome AskAiSendNow(const QueuedAgentSend& item) = 0;
  /** The demo's made-up answers: the question is stored here; its message id ("" when it failed). */
  virtual std::string AskAiStoreQuestion(const std::string& thread_id, const std::string& text) = 0;
  /** Pushes the sheet; `on_closed` runs once it is closed by any means. Returns its layer id (-1: none). */
  virtual int AskAiPushSheet(const PaneSpec& spec, std::function<void()> on_closed) = 0;
  virtual void AskAiCloseSheet(int layer_id) = 0;
  /** Shows the thread in the full chat page (the panel is closed first). */
  virtual void AskAiOpenInChat(const std::string& thread_id) = 0;
  /** Runs `fn` on the UI thread after the event being handled (the sheet's own button may close the sheet). */
  virtual void AskAiLater(std::function<void()> fn) = 0;
  /** A button of a stored answer (a permission card, a link button): `action_index` of that message. */
  virtual void AskAiRunAction(const std::string& thread_id, const std::string& entry_id, int action_index) = 0;
  /** Link `link_index` of a stored answer of the thread. */
  virtual void AskAiOpenLink(const std::string& thread_id, const std::string& entry_id, int link_index) = 0;
  virtual bool AskAiIntroSeen() const = 0;
  virtual void AskAiMarkIntroSeen() = 0;
  virtual void AskAiToast(const std::string& text) = 0;
  /** The emoji sheet for the panel's input: `insert` runs with each emoji picked. */
  virtual void AskAiOpenEmoji(std::function<void(const std::string& emoji)> insert) { (void)insert; }

  // --- the PP 客服 mode (a settings "?") -------------------------------------------------------------------
  /** Messaging is up (just after unlocking it is not yet: nothing can be found or sent). */
  virtual bool AskAiMessagingReady() const = 0;
  /** The PP 客服 chat, found or made, not opened (the page under the sheet stays); an error with the user's words. */
  virtual Roe<std::string> AskAiEnsureSupport() = 0;
  /** `text` into that chat, as a message typed there (stored and sent at once). */
  virtual Roe<void> AskAiSendToSupport(const std::string& thread_id, const std::string& text) = 0;
  /** The chat's last `limit` messages as the support decisions read them (SupportMessageOf), oldest first. */
  virtual std::vector<SupportMessage> AskAiSupportMessages(const std::string& thread_id, size_t limit) = 0;
  /** Fetches the inbox now (a reply is awaited; without a push it comes only by fetching). */
  virtual void AskAiSyncInbox() = 0;
  /** The user is looking at the app (AppLifecycle::IsUserAttentive). */
  virtual bool AskAiAttentive() const = 0;
  /** The chat is read (its unread count and notifications go): a reply the sheet shows to someone looking. */
  virtual void AskAiMarkRead(const std::string& thread_id) = 0;
};

/**
 * The ask-AI sheet (DEV/PLAN-ui-batch5-ask-ai.md 5a, design §8.14): opens over the page the user is on, which
 * stays where it was. It shows the shared AI conversation, the AI chat used last (Home and the panel continue it; New
 * chat's "AI chat" makes a new one, which is then the last), and sends into it through the same router and queue as
 * the chat page (the sessions' cap, a guest's 1). A setting's "?" shows the PP 客服 chat instead (OpenSupport,
 * DEV/PLAN-help-to-support-panel.md): its question goes to PP 客服, nothing goes to the AI.
 *
 * Closing keeps the unsent text and lets a running answer finish where it belongs; opening again sends nothing.
 *
 * UI thread.
 */
class AskAiPanel {
public:
  /** At most this many messages of the shared conversation show (the full chat has the rest). */
  static constexpr size_t kRowLimit = 30;

  AskAiPanel(AskAiPanelHost& host, AgentTurnController& agent);
  ~AskAiPanel();

  bool Register(ui::Context* context);

  // --- entries (false: not on a phone, nothing opened) ----------------------------------------------
  bool Open(AskAiEntry entry);
  /** Opens and asks `question` in the shared conversation (a Home chip); a question already waiting is not sent twice. */
  bool OpenAndAsk(AskAiEntry entry, const std::string& question);
  /**
   * Opens the PP 客服 chat (a settings "?") and sends `question` into it ("" = just open), unless the same question of
   * ours went out less than 10 minutes ago (SupportAsk). An open AI sheet is closed first: the modes never switch in
   * place. When PP 客服 cannot be reached the sheet stays open and says why; nothing is sent.
   */
  bool OpenSupport(const std::string& question);

  // --- the panel's buttons ---------------------------------------------------------------------------
  void Send();
  void DismissIntro();
  /** "Ask PP Support" under an AI answer that never came: the sheet closes and opens again on PP 客服 with it. */
  void AskSupport();
  /** The intro card's "?": the sheet closes and opens again on PP 客服, asking how to use one's own AI. The card stays
   *  unseen. */
  void AskSupportOwnAi();
  void OpenInChat();
  /** Closes the sheet (the draft stays; a running answer goes on). */
  void Close();
  void RunAction(const std::string& entry_id, int action_index);
  void OpenLink(const std::string& entry_id, int link_index);
  /** The camera / the attachment button: one picture for the next question. */
  void PickPicture(bool camera);
  /** The bold plus (Kenneth 2026-11-07): the chat composer's floating Camera / Photos / File card above `anchor`. */
  void ShowAttachMenu(ui::Element* anchor);
  /** The emoji key: an emoji picked in the sheet goes at the end of the draft. */
  void InsertEmoji(const std::string& emoji);
  void RemovePicture();

  // --- from the chat screen -------------------------------------------------------------------------
  /** Per frame: the streamed answer, the PP 客服 waiting line, the demo's answers, focus, scroll. */
  void Tick();
  void OnTurnEnded(const std::string& thread_id);
  /**
   * Messages were stored or changed (a reply came, ours went out): the PP 客服 chat shown is drawn again, and read when
   * someone is looking (SupportAsk), so no unread is left in the chats list. Called first in each messages refresh.
   */
  void OnMessagesChanged();
  /** No notification for a new message in `thread_id`: the sheet shows it to someone looking (SupportAsk). */
  bool SuppressesNoticeFor(const std::string& thread_id) const;

  bool IsOpen() const { return layer_id_ >= 0; }

  // --- copying -----------------------------------------------------------------------------------------
  /** What a bubble shows, as plain text: no markup, one line per block. `row` is the bubble's row element. */
  static std::string PlainTextOf(ui::Element* row);
  /**
   * The menu of a long press / right click on a bubble: just "Copy" (the text selection if there is one, else the
   * bubble's text). nullopt for a press outside the bubbles.
   */
  std::optional<std::vector<ContextMenuAction>> TextMenuFor(const ContextMenuRequest& request);
  /** The panel is open on `thread_id` (an answer there is read). */
  bool Shows(const std::string& thread_id) const;
  /** The shared conversation the panel last showed. */
  const std::string& SharedThread() const { return shared_thread_; }
  const AskAiView& View() const { return view_; }
  /** The UI demo app (no AI backend): answers come from `answer` ("" = it fails), streamed in small steps. */
  void SetFakeAnswers(std::function<std::string(const std::string& question)> answer) {
    fake_answer_ = std::move(answer);
  }
  AskAiImagePorts& ImagePorts() { return image_ports_; }
  /** Tests: the panel's draft as if typed. */
  void SetDraftForTest(const std::string& text) { view_.draft = text.c_str(); }
  /** Tests: the clock the PP 客服 waiting line and fetches go by (unix ms). */
  void SetClockForTest(std::function<int64_t()> now_ms) { now_ms_ = std::move(now_ms); }

private:
  struct FakeStream {
    std::string thread_id;
    std::string answer; // "" = the turn fails
    std::chrono::steady_clock::time_point started;
    size_t shown = 0;
  };

  /** The PP 客服 mode's own state (the decisions are SupportAsk's). */
  struct SupportState {
    std::string thread;
    std::string draft;
    /** What ReplyState reads, from the last time the rows were drawn. */
    std::vector<SupportMessage> messages;
    /** A question asked from here waits for its reply (the waiting line follows ReplyState). */
    bool awaiting = false;
    int64_t next_fetch_ms = 0;
    int64_t fetch_until_ms = 0;
    /** Messaging was not up yet: this question is asked once it is (one retry), until `ready_by_ms`. */
    std::optional<std::string> pending;
    int64_t ready_by_ms = 0;
  };

  bool Show(AskAiEntry entry, bool support);
  /** The thread a question goes to: the shared one, or (a fresh dot panel) the new one, made on the first question. */
  std::string ThreadToSend();
  void SwitchMode(bool support);
  /** The unsent text kept for the mode shown. */
  std::string& ModeDraft();
  void StartSupport(const std::string& question);
  /** Sends into the PP 客服 chat; a "?"'s question (not `typed`) only when SupportAsk says it is not a repeat. */
  void AskSupportQuestion(const std::string& text, bool typed);
  /** A reply is awaited from `now`: the waiting line follows it and the inbox is fetched anew for a while. */
  void StartSupportWait(int64_t now);
  void SyncSupportWait();
  /** The PP 客服 chat shown is read when someone looks at it (SupportAsk::ShouldReadSupportThread). */
  void ReadSupportIfLooking();
  /** Per frame in PP 客服 mode: the retry once messaging is up, the 90 s line, the fetches while waiting. */
  void TickSupport();
  void SendShared(const std::string& text);
  void SendPicture(const std::string& text);
  void PreparePicture(std::string path);
  void DropPicture();
  void SyncPictureView();
  void StartFake(const std::string& thread_id, const std::string& question);
  void TickFake();
  void FailShared(const std::string& question);
  /** Rebuilds the rows (and what goes with them) from the conversation shown; dirties what changed. */
  void Refresh();
  void RefreshStreaming(bool force);
  void Dirty(const char* field);
  void DirtyAll();
  /** The input bar floats (frosted) over the bubbles: the scroll's end spacer is kept as tall as the bar. */
  void SyncComposerClear();

  AskAiPanelHost& host_;
  AgentTurnController& agent_;
  ui::Context* context_ = nullptr;
  AskAiView view_;
  int layer_id_ = -1;

  AskAiImagePorts image_ports_;
  std::optional<AgentTurnController::PendingAiImage> picture_;
  bool picture_preparing_ = false;
  uint64_t picture_generation_ = 0;

  std::string shared_thread_;
  /**
   * The panel was opened from the floating dot: it starts empty, and its first question makes a new AI chat
   * (`shared_thread_`), which the panel keeps until it is opened again. Nothing is made if no question is sent.
   */
  bool fresh_ = false;
  std::string shared_draft_;
  /** A shared question was handed over and its answer is awaited (an end without one is an error). */
  bool awaiting_answer_ = false;
  bool shared_failed_ = false;
  /** The shared question handed over last, and the one whose answer never came (the support button asks it). */
  std::string asked_question_;
  std::string failed_question_;

  SupportState support_;
  std::function<int64_t()> now_ms_;

  std::function<std::string(const std::string&)> fake_answer_;
  std::optional<FakeStream> fake_;

  std::vector<AskAiRow> base_rows_;
  std::string streamed_text_;
  std::optional<std::chrono::steady_clock::time_point> last_stream_render_;
  int scroll_frames_ = 0;
};

} // namespace pbr
