#pragma once

#include "feature/ai/AgentSession.h"
#include "feature/ai/AgentUiPorts.h"
#include "feature/conversations/ConversationsFacade.h"
#include "foundation/data/Config.h"
#include "gui/chat/AgentBusyRules.h"
#include "gui/chat/ChatScreenState.h"
#include "gui/chat/ChatWidgetHost.h"
#include "gui/chat/HeldAgentSend.h"
#include "gui/chat/WorkingSetController.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/ai/BriefAiClient.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/TurnPlan.h"
#include "lib/messaging/AtAiParser.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include "lib/ui/ContextMenuHost.h"

#include <functional>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class SessionStore;
class ToolRegistry;

/** Pseudo thread whose blobs_view holds AI image thumbnails; wiped with the other session plaintext views. */
inline constexpr const char* kAiImageViewThread = "ai-images";
/**
 * The ask-AI panel's one-off question (settings "?", DEV/PLAN-ui-batch5-ask-ai.md 5a): it runs in the Home session
 * and shows only in the panel, under this pseudo thread ("busy", the streamed text). Never a stored thread.
 */
inline constexpr const char* kAskAiOnceThread = "ask-ai-once";

/** What AgentTurnController needs from the chat screen it is part of (ChatController). UI thread. */
class AgentTurnHost {
public:
  virtual ~AgentTurnHost() = default;
  virtual ConversationsFacade* Facade() const = 0;
  /** Messaging is wired to this screen (vault unlocked, bindings in place). */
  virtual bool MessagingIsReady() const = 0;
  virtual bool MessagingInitialized() const = 0;
  virtual bool MessagingReady() const = 0;
  virtual const std::string& ActiveThreadId() const = 0;
  virtual ShellChromeSnapshot ChromeSnapshot() const = 0;
  virtual bool InAiComposerContext() const = 0;
  virtual void SyncDisplayFromThread() = 0;
  virtual void SyncShellSessions() = 0;
  virtual void SyncComposerInputState() = 0;
  virtual void UpdateSidebarPreview(const std::string& preview_text) = 0;
  virtual bool EnsureHomeOutboundSession(const std::string& chip_message = {}) = 0;
  virtual void SendUserText(const std::string& text, std::optional<std::string> user_payload = std::nullopt,
                            bool from_home_chip = false) = 0;
  virtual void ShellSetActivity(bool visible, const ui::String& message = {}) = 0;
  virtual void ShowAttachChoice(std::function<void()> photo, std::function<void()> file) = 0;
  virtual void RegisterMessagingTools(ToolRegistry& tools) = 0;
  virtual SessionStore& Store() = 0;
  /** A local action a reply's button asked for (tool permission, open a URL, …); `entry_id` = the tapped message ("" = unknown). */
  virtual void HandleLocalAction(const std::string& message, const std::optional<std::string>& payload,
                                 const std::string& entry_id) = 0;
  /** Send a queued AI question into its own thread (which need not be the open one) now that the AI is free. */
  virtual QueuedSendOutcome RunQueuedAgentSend(const QueuedAgentSend& item) = 0;
  /** Unsent text goes back to the composer of the conversation it was typed in (not the open one, if different). */
  virtual void PutBackDraft(const std::string& thread_id, const std::string& text) = 0;
  /** A turn of `thread_id` ended: what it stored (the answer) is in the thread now. */
  virtual void OnAgentTurnEnded(const std::string& thread_id) { (void)thread_id; }
  /** The ask-AI panel shows this thread now: an answer there is read, not unread. */
  virtual bool AskAiPanelShows(const std::string& thread_id) const {
    (void)thread_id;
    return false;
  }
  /** The finished answer of the panel's one-off question (kAskAiOnceThread): never stored in a thread. */
  virtual void OnAskAiOnceReply(const AgentEvent& event) { (void)event; }
};

/**
 * The chat screen's agent side: the assistant's configuration and readiness (and the LLM setup
 * banner), its turns (events, the streamed answer, the finished reply with its widgets and working
 * set, Stop), a send held while it connects, the image attached to an AI question, the agent's
 * forms / calendars, and chat actions. ChatController owns the data model and routes the clicks.
 *
 * UI thread.
 */
class AgentTurnController : public Module {
public:
  /** Hot-reloadable assistant slice projected from AppConfig (LLM / MCP / search). */
  struct AgentConfig {
    LlmConfig llm;
    std::string llm_api_key_env;
    McpConfig promoted_mcp;
    std::vector<McpConfig> mcp_servers;
    SearchConfig search;
    ContextBudget context = DefaultContextBudget();

    bool operator==(const AgentConfig& other) const {
      if (llm.base_url != other.llm.base_url || llm.model != other.llm.model ||
          llm.api_key != other.llm.api_key || llm.preset != other.llm.preset ||
          llm.require_api_key != other.llm.require_api_key ||
          llm_api_key_env != other.llm_api_key_env || promoted_mcp.url != other.promoted_mcp.url ||
          search.provider != other.search.provider || mcp_servers.size() != other.mcp_servers.size()) {
        return false;
      }
      for (size_t i = 0; i < mcp_servers.size(); ++i) {
        const McpConfig& a = mcp_servers[i];
        const McpConfig& b = other.mcp_servers[i];
        if (a.id != b.id || a.url != b.url || a.command != b.command || a.enabled != b.enabled ||
            a.args != b.args) {
          return false;
        }
      }
      return true;
    }
    bool operator!=(const AgentConfig& other) const { return !(*this == other); }
  };
  static AgentConfig ProjectAgent(const AppConfig& config);

  AgentTurnController(AgentTurnHost& host, ChatScreenState& chat, std::map<std::string, std::vector<std::string>>& chat_links,
                      ChatWidgetHost& widgets, WorkingSetController& working_set, ShellFeedbackPorts& shell_feedback);

  // --- the agent -------------------------------------------------------------------------------
  void BindPorts(AgentUiPorts ports);
  AgentUiPorts& Ports() { return agent_ports_; }
  void ApplyConfig(const AgentConfig& config);
  void ReloadAgentConfig();
  /** An LLM is configured (else replies are the mock assistant's). */
  bool UseLlm() const { return use_llm_; }
  void SetUseLlm(bool use) { use_llm_ = use; }
  bool AgentReady() const;
  bool AgentConfigured() const;
  /** Agent session + cloud/mock LLM usable (Brief/API key resolved). Peer relay ignores this. */
  bool AgentCloudReady() const;
  /** The Brief / API key the agent needs is there (AgentCloudReady minus the configured check). */
  bool AgentHasCredentials() const;
  /** Mint/cache free-tier Brief guest Bearer when registered key is missing. */
  void EnsureBriefGuestLlmKey();
  /** Show/clear LLM setup banners once identity is readable (after unlock). */
  void RefreshLlmSetupBanner();

  // --- turns -----------------------------------------------------------------------------------
  /** Per frame: the streamed row, a held send, the agent's events. */
  void Tick();
  void HandleAgentEvent(const AgentEvent& event);
  /** Stores an answer in `thread_id`, the chat its turn belongs to; with none it is dropped (never the open chat). */
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                            const std::string& finish_reason = {}, const std::string& thread_id = {},
                            ResponseGoal response_goal = ResponseGoal::General,
                            RenderMode render_mode = RenderMode::Blocks, AtAiMode shared_ai_mode = AtAiMode::None,
                            const std::vector<BriefAiSource>& sources = {});
  /** The stop key / a double Escape: stops the turn of the open thread only. */
  void OnStopTurn();
  /**
   * The Home composer's question (no messaging router): busy in `thread_id` once the session took it; if it did
   * not (its turn still runs), the text goes back to that composer.
   */
  void SubmitHomeQuestion(const std::string& thread_id, const std::string& text, std::optional<std::string> payload);
  /**
   * The ask-AI panel's one-off question in the Home session (its conversation started empty first), answered in
   * words only (no app action, no permission card): busy in kAskAiOnceThread once taken. Its events reach only the
   * panel (OnAskAiOnceReply), never a chat.
   */
  SubmitOutcome SubmitAskAiOnce(const std::string& text);
  /** The answer streaming in `thread_id` so far ("" when none). */
  std::string StreamingText(const std::string& thread_id) const;
  /** The question waiting in the queue for `thread_id` ("" when none). */
  std::string QueuedText(const std::string& thread_id) const;

  // --- several turns at once, "busy" per thread, a queue once the sessions are full (B112) -------
  const AgentBusyState& Busy() const { return busy_; }
  /**
   * May an AI send start in `thread_id` now: BusyHere while the screen shows a turn there, else the sessions' answer
   * (the mock assistant answers one at a time).
   */
  TurnStart CanStart(const std::string& thread_id) const;
  /** A send was handed to the AI for `thread_id`: busy now, before LoadingChanged comes back. */
  void MarkAgentBusy(const std::string& thread_id);
  void MarkAgentIdle(const std::string& thread_id);
  /** Recompute what the open thread shows: thinking + stop key, the queued bubble, the status bar activity. */
  void RefreshBusyForActiveThread();
  bool HasQueued(const std::string& thread_id) const { return queue_.Has(thread_id); }
  bool QueueSend(QueuedAgentSend item);
  std::optional<QueuedAgentSend> TakeQueued(const std::string& thread_id);
  /** A turn ended: hand over queued sends, in order, while the sessions can start them. */
  void DrainQueue();
  size_t QueuedCount() const { return queue_.Size(); }

  /** Stop every turn in flight (the app pauses). */
  void Cancel();
  /**
   * A conversation is being deleted: stop its turn, drop its queued send and streamed row, and ignore any event of it
   * that still arrives, so a late answer never stores a message into (and so brings back) the deleted thread.
   */
  void ForgetThread(const std::string& thread_id);
  /** The deleted-conversation list is per profile: emptied on a profile reset / switch (and in `Reset`). */
  void ClearForgottenThreads();
  /**
   * A permission card ended (answered, given up, or found stale on a tap): rewrite that one stored message by its
   * id so its buttons are gone. `thread_id` empty = the open thread.
   */
  void CloseApprovalCard(const std::string& thread_id, const std::string& entry_id, ApprovalOutcome outcome,
                         const std::string& approval_id = {});
  /** A send made while the agent was still (re)configuring: sent once it is, or given up. One per thread. */
  void HoldSend(HeldAgentSend::Send send) { held_agent_send_.Hold(std::move(send), std::chrono::steady_clock::now()); }
  void SendChatAction(const std::string& entry_id, int action_index);
  void SendSharedAssistantRelay(const std::string& thread_id, AtAiMode mode, const std::string& plain_text);
  /** Drop what a turn left (the profile / app is going away). */
  void Reset();

  // --- an image for the AI ---------------------------------------------------------------------
  /** The prepared image waiting in the composer for its question. */
  struct PendingAiImage {
    BriefAiImage image;
    std::string name;      // shown on the chip only; never logged
    std::string file_path; // session plaintext copy for the thumbnail; empty when it could not be written
    int width = 0;         // pixels of the prepared image
    int height = 0;
  };
  void OnAttachAiImage();
  /** The Home composer's plus: a floating menu above it with Camera and Photos (no file item); each hands its image to
   *  StartAiImagePrepare like a picked photo. */
  void ShowHomeAttachMenu(ui::Element* anchor);
  /** The Home plus menu's rows, in order: Camera, then Photos. Both are always offered (a missing camera is told by the
   *  capture itself). Pure, so a test can count them. */
  static std::vector<ContextMenuAction> HomeAttachActions(std::function<void()> camera, std::function<void()> photos);
  void StartAiImagePrepare(std::string path);
  /** Drops the pending (or still preparing) image and its session thumbnail file. */
  void DiscardPendingAiImage();
  /** Leaving a conversation: its ready image is kept for it (a still-preparing one is dropped); the chip clears. */
  void StashPendingImage(const std::string& key);
  /** Back in a conversation: the image kept for it is in the composer again. */
  void RestorePendingImage(const std::string& key);
  /** The conversation is gone (closed / cleared): its kept image and thumbnail file go too. */
  void ForgetPendingImage(const std::string& key);
  void ForgetAllPendingImages();
  bool HasPendingImage() const { return pending_image_.has_value(); }
  /** False when the question could not be sent (not ready, no thread); the draft and the chip then stay. */
  bool SendImageQuestion(const std::string& text);
  /**
   * The image is stored (or failed to be): hand the question to the session. If it does not take it, the picture
   * and `text` go back to `thread_id`'s composer and "busy" there ends. Public for tests.
   */
  void SubmitStoredImageQuestion(const std::string& thread_id, const std::string& text, const std::string& question,
                                 const std::string& message_id, PendingAiImage image, std::string ai_image_json);
  /** `thread_id` empty: the open thread (the chat page); the ask-AI panel names its own. */
  void DecorateAiImageRows(std::vector<MessageDisplayRow>& rows, const std::string& thread_id = {}) const;

  // --- the ask-AI panel's picture (DEV/PLAN-ui-batch5-ask-ai.md 5b): the same path, its own state ---------
  /** The photo library / file picker (images only); `picked` runs on the UI thread with one file, or not at all. */
  void PickAiImage(std::function<void(std::string path)> picked);
  /** The system camera, photo only; `picked` as above. Tells the person when the camera is unavailable. */
  void CaptureAiPhoto(std::function<void(std::string path)> picked);
  /** PrepareAiImageFromFile (<= 1.5 MB, metadata stripped) + a session thumbnail, off the UI thread; `done` on it. */
  void PrepareAiImage(std::string path, std::function<void(Roe<PendingAiImage>)> done);
  /** Removes the picture's session thumbnail file. */
  void DropAiImage(const PendingAiImage& image);
  enum class ImageSend { Started, Full, BusyHere, Unavailable };
  /** Stores the picture in `thread_id` and starts its question there (not queued: the picture lives in the panel). */
  ImageSend SendAiImageTo(const std::string& thread_id, const std::string& text, PendingAiImage image);

  // --- the agent's forms and calendars ---------------------------------------------------------
  void SubmitForm(const std::string& entry_id, const std::string& form_id);
  void CalendarPrev(const std::string& entry_id);
  void CalendarNext(const std::string& entry_id);
  void SelectCalendarDay(const std::string& entry_id, const std::string& iso_date);

private:
  /** The tail of SendImageQuestion: names the thread, marks it busy, stores the picture, hands the question over. */
  void StartPreparedImageSend(const std::string& thread_id, const std::string& text, PendingAiImage image);
  /** A people list's "show more": `step` more rows of `blocks[block_index]` of that stored reply, in place. */
  void ShowMoreRows(const std::string& entry_id, size_t block_index, size_t step);
  void OnAssistantDelta(const AgentEvent& event);
  /** Renders the streamed rows (each at most every 100 ms) and puts the open thread's in its messages. */
  void FlushStreamingRow();
  void ClearStreamingRow(const std::string& thread_id);
  /** The thread an event belongs to: its own, or (none: the Home session's) the chat the Home question came from. */
  std::string EventThread(const AgentEvent& event) const;
  /** Sends the held send once the agent is configured, or gives it up after kHeldAgentSendTimeout. Per frame. */
  void FlushHeldAgentSend();

  AgentTurnHost& host_;
  ChatScreenState& chat_;
  /** entry id -> https links its rendered bubble refers to (open_chat_link index). */
  std::map<std::string, std::vector<std::string>>& chat_links_;
  ChatWidgetHost& widgets_;
  WorkingSetController& working_set_;
  ShellFeedbackPorts& shell_feedback_;

  AgentUiPorts agent_ports_;
  AgentConfig last_agent_runtime_;
  bool use_llm_ = false;
  bool brief_guest_mint_attempted_ = false;
  std::string brief_guest_mint_user_hint_;
  std::string last_brief_banner_; // text RefreshLlmSetupBanner last showed for the brief preset
  HeldAgentSend held_agent_send_; // sends made while the agent was still (re)configuring, one per thread
  AgentBusyState busy_;           // the threads whose turn runs (B112)
  AgentSendQueue queue_;          // AI sends waiting for a free turn
  bool draining_ = false;
  /** The queue's head was last kept waiting (logged once until something is handed over). */
  bool queue_held_ = false;
  /** The last step label ("Searching the web…") of each running turn, shown in its own thread. */
  std::map<std::string, std::string> step_status_;
  /**
   * Per thread: when the UI was busy there but the session not. After kBusyRecoveryDelay the UI stops waiting for an
   * end it missed.
   */
  std::map<std::string, std::chrono::steady_clock::time_point> busy_without_session_since_;
  void RecoverMissedTurnEnd();
  /** The queue is not empty and nothing is marked busy: DrainQueue again, at most once a second. */
  void RetryQueueDrain();
  std::optional<std::chrono::steady_clock::time_point> last_drain_retry_;
  /**
   * The chat a Home composer's turn (SubmitHomeQuestion) shows in: stop and recovery ask the Home session, and its
   * events (which carry no thread) belong there.
   */
  std::optional<std::string> home_turn_thread_;
  /**
   * The Home session's newest turn the UI knows (AgentEvent::turn): a Home event of an older turn is a stopped turn's
   * late one and is dropped (it must not end, or answer for, the turn that runs now).
   */
  uint64_t home_turn_ = 0;
  void NoteHomeTurnStarted();

  std::optional<PendingAiImage> pending_image_;
  std::map<std::string, PendingAiImage> stashed_images_; // per conversation, while another one is shown
  uint64_t image_prepare_generation_ = 0; // a prepare that finishes after Discard / a newer pick is dropped

  /** An in-flight streamed answer: shown as a synthetic last row of its thread until AssistantReady replaces it. */
  struct StreamingRow {
    std::string row_id; // "streaming-<user message id>"; also the __ENTRY__ of its links
    std::string thread_id;
    std::string text;
    std::string rml;
    bool dirty = false;
    std::optional<std::chrono::steady_clock::time_point> last_render;
  };
  std::map<std::string, StreamingRow> streaming_; // by thread
  std::set<std::string> forgotten_threads_;        // deleted conversations: their late events are dropped
};

} // namespace pbr
