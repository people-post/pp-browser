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
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class SessionStore;
class ToolRegistry;

/** Pseudo thread whose blobs_view holds AI image thumbnails; wiped with the other session plaintext views. */
inline constexpr const char* kAiImageViewThread = "ai-images";

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
  void FinishAssistantReply(const std::string& entry_id, const std::string& raw_output, bool from_llm,
                            const std::string& finish_reason = {}, const std::string& thread_id = {},
                            ResponseGoal response_goal = ResponseGoal::General,
                            RenderMode render_mode = RenderMode::Blocks, AtAiMode shared_ai_mode = AtAiMode::None,
                            const std::vector<BriefAiSource>& sources = {});
  /** The stop key / a double Escape: stops the turn only if it runs in the open thread. */
  void OnStopTurn();

  // --- one turn at a time, "busy" per thread, a queue for the others (B112) -----------------------
  const AgentBusyState& Busy() const { return busy_; }
  /** A send was handed to the AI for `thread_id`: busy now, before LoadingChanged comes back. */
  void MarkAgentBusy(const std::string& thread_id);
  void MarkAgentIdle();
  /** Recompute what the open thread shows: thinking + stop key, the queued bubble, the status bar activity. */
  void RefreshBusyForActiveThread();
  bool HasQueued(const std::string& thread_id) const { return queue_.Has(thread_id); }
  bool QueueSend(QueuedAgentSend item);
  std::optional<QueuedAgentSend> TakeQueued(const std::string& thread_id);
  /** The AI is free: hand over queued sends until one starts a turn (one that does not is skipped). */
  void DrainQueue();
  size_t QueuedCount() const { return queue_.Size(); }

  /** Stop a turn in flight (pause, a new chat). */
  void Cancel();
  /**
   * A permission card ended (answered, given up, or found stale on a tap): rewrite that one stored message by its
   * id so its buttons are gone. `thread_id` empty = the open thread.
   */
  void CloseApprovalCard(const std::string& thread_id, const std::string& entry_id, ApprovalOutcome outcome);
  /** A send made while the agent was still (re)configuring: sent once it is, or given up. */
  void HoldSend(HeldAgentSend::Send send) { held_agent_send_.Hold(std::move(send), std::chrono::steady_clock::now()); }
  void SendChatAction(const std::string& entry_id, int action_index);
  void SendSharedAssistantRelay(const std::string& thread_id, AtAiMode mode, const std::string& plain_text);
  /** Drop what a turn left (the profile / app is going away). */
  void Reset();

  // --- an image for the AI ---------------------------------------------------------------------
  void OnAttachAiImage();
  void StartAiImagePrepare(std::string path);
  /** Drops the pending (or still preparing) image and its session thumbnail file. */
  void DiscardPendingAiImage();
  bool HasPendingImage() const { return pending_image_.has_value(); }
  /** False when the question could not be sent (not ready, no thread); the draft and the chip then stay. */
  bool SendImageQuestion(const std::string& text);
  void DecorateAiImageRows(std::vector<MessageDisplayRow>& rows) const;

  // --- the agent's forms and calendars ---------------------------------------------------------
  void SubmitForm(const std::string& entry_id, const std::string& form_id);
  void CalendarPrev(const std::string& entry_id);
  void CalendarNext(const std::string& entry_id);
  void SelectCalendarDay(const std::string& entry_id, const std::string& iso_date);
  std::string HydrateAssistantRml(const TranscriptEntry& entry) const;
  bool IsFormEditable(const std::string& entry_id, const std::string& form_id) const;
  void InitializeWidgetState(const std::string& entry_id, const std::vector<WidgetInit>& inits);
  void MergeWidgetStateIntoRow(const std::string& entry_id, TranscriptDisplayRow& row) const;
  TurnWidgetState* FindWidgetState(const std::string& entry_id);
  const TurnWidgetState* FindWidgetState(const std::string& entry_id) const;
  void ClearFormState();

private:
  /** A people list's "show more": `step` more rows of `blocks[block_index]` of that stored reply, in place. */
  void ShowMoreRows(const std::string& entry_id, size_t block_index, size_t step);
  void OnAssistantDelta(const AgentEvent& event);
  void FlushStreamingRow();
  void ClearStreamingRow();
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
  HeldAgentSend held_agent_send_; // a send made while the agent was still (re)configuring
  AgentBusyState busy_;           // the one running turn and its thread (B112)
  AgentSendQueue queue_;          // AI sends of other threads, waiting for it
  bool draining_ = false;

  /** The prepared image waiting in the composer for its question. */
  struct PendingAiImage {
    BriefAiImage image;
    std::string name;      // shown on the chip only; never logged
    std::string file_path; // session plaintext copy for the thumbnail; empty when it could not be written
    int width = 0;         // pixels of the prepared image
    int height = 0;
  };
  std::optional<PendingAiImage> pending_image_;
  uint64_t image_prepare_generation_ = 0; // a prepare that finishes after Discard / a newer pick is dropped

  /** The in-flight streamed answer: shown as a synthetic last row until AssistantReady replaces it. */
  struct StreamingRow {
    std::string row_id; // "streaming-<user message id>"; also the __ENTRY__ of its links
    std::string thread_id;
    std::string text;
    std::string rml;
    bool dirty = false;
    std::optional<std::chrono::steady_clock::time_point> last_render;
  };
  std::optional<StreamingRow> streaming_;
};

} // namespace pbr
