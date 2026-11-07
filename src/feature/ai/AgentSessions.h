#pragma once

#include "feature/ai/AgentSession.h"
#include "common/Module.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Thread turns that may run at the same time for a registered user; the Home composer's turn is not counted
 * (DEV/PLAN-ai-concurrent-turns.md). Setting it back to 1 restores one answer at a time.
 */
inline constexpr size_t kMaxConcurrentTurns = 3;
/**
 * The same for a guest (`brf_guest_*` key): the gateway allows a guest few requests a minute, and each turn may take
 * two (a retry when brief_AI is busy).
 */
inline constexpr size_t kMaxConcurrentGuestTurns = 1;
/**
 * Live sessions (Home and the spare included) above which the least recently used idle one is closed at once.
 * Never refuses: if none can close, one more is opened.
 */
inline constexpr size_t kSoftMaxLiveSessions = 6;
/** A thread's session that could close (AgentSession::CanClose) is closed after this long without a turn. */
inline constexpr std::chrono::minutes kSessionIdleTimeout{10};
/** How long closing the sessions waits for stopped turns to leave their threads. */
inline constexpr std::chrono::milliseconds kTurnStopBudget{2000};
/**
 * Threads for turns: the most thread turns at once (any key), Home's, and two for a turn's next step (a tool
 * dispatch, a resumed card, a start after configure) and stopped turns still leaving, so none waits for a thread.
 */
inline constexpr size_t kTurnPoolThreads = kMaxConcurrentTurns + 1 + 2;

/**
 * The most thread turns at once with this configuration: kMaxConcurrentTurns for a registered brief_AI key
 * (`brf_llm_*`) or another provider's key, kMaxConcurrentGuestTurns for a guest key or none yet (the guest key is
 * minted after launch).
 */
size_t MaxConcurrentTurnsFor(const AppConfig& config);

/** May a turn start in a thread now? */
enum class TurnStart {
  Yes,
  Full,     // as many thread turns run as allowed
  BusyHere, // a turn already runs in this thread
};

/**
 * The assistant's sessions: one fixed session for the Home composer (Conversation mode, no thread) and one per
 * thread, taken when that thread first asks and closed once it has been idle for kSessionIdleTimeout. A thread's
 * turns always run in its own session, so its permission card, a question the app asked back and its events stay
 * with it. How many thread turns run at once is decided here and only here (`Submit*` answer Full).
 *
 * One spare session is kept configured and idle, so a thread's first question does not wait for a whole configure
 * (MCP start included): a new thread takes it and the next spare starts configuring at once.
 *
 * Settings (configuration, tool hook, permissions, thread store, ...) reach every live session and every one made
 * later. The events of all sessions come out of one queue (PollEvents) in the order they were posted; each carries
 * its thread. The brief_AI role that answered a chat last is kept here too, so a rebuilt session still knows it.
 *
 * UI thread (as AgentSession's own Submit*).
 */
class AgentSessions : public Module {
public:
  using Clock = std::function<std::chrono::steady_clock::time_point()>;

  /**
   * `max_concurrent_turns` and `clock` are for tests: a fixed cap instead of the configuration's, and the clock that
   * times idle sessions.
   */
  explicit AgentSessions(std::optional<size_t> max_concurrent_turns = std::nullopt, Clock clock = {});
  /** Stops every turn and waits briefly for their threads. */
  ~AgentSessions();

  AgentSessions(const AgentSessions&) = delete;
  AgentSessions& operator=(const AgentSessions&) = delete;

  // --- settings of every session -------------------------------------------------------------
  void Configure(const AppConfig& config);
  void SetToolRegistrationHook(ToolRegistrationHook hook);
  void SetToolPermissions(ToolPermissionsPrefs permissions);
  void SetToolPermissionsSaver(ToolPermissionsSaveFn saver);
  void SetThreadStore(IThreadStore* store);
  void SetLocalContactsSource(LocalContactsFn source);
  /** Tests / a fake server: the brief_AI stream of every session. */
  void SetBriefAiStream(BriefAiStreamFn fn);
  /** Every session: how a brief turn renews its key after HTTP 401 (AgentSession::SetBriefKeyRenewer). */
  void SetBriefKeyRenewer(BriefKeyRenewFn renew);

  // --- turns -----------------------------------------------------------------------------------
  /** An empty `thread_id` is the Home composer: always Yes unless its own turn runs. */
  TurnStart CanStart(const std::string& thread_id) const;
  /** The Home composer's question; never Full. */
  SubmitOutcome Submit(const std::string& user_text, std::optional<std::string> user_payload = std::nullopt);
  SubmitOutcome SubmitToThread(const std::string& thread_id, const std::string& user_text,
                               std::optional<std::string> user_payload = std::nullopt,
                               std::optional<AgentImageTurn> image = std::nullopt);
  SubmitOutcome SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                                   std::optional<std::string> user_payload = std::nullopt,
                                   AtAiMode mode = AtAiMode::Local);
  /** An empty `thread_id` runs in the Home session. */
  SubmitOutcome SubmitLocalAction(const std::string& thread_id, const std::string& user_text, const std::string& tool,
                                  Object args, bool in_peer_chat = false);
  /**
   * In the session holding that card; resuming starts a turn there, so a thread's card counts like a turn from the
   * tap on (TurnBusy when full). A Home card does not count, like any Home turn.
   */
  Roe<void> ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                 const std::string& decision_label = {});
  bool IsAppActionRequest(const std::string& text) const;
  /** The events of every session. Also closes sessions idle for kSessionIdleTimeout. Per frame. */
  void PollEvents(std::vector<AgentEvent>& out);

  /** Stops the turn of `thread_id` only (empty: the Home composer's). */
  void Cancel(const std::string& thread_id);
  /** Stops every turn (the app pauses or quits). */
  void CancelAll();
  /** A turn runs in `thread_id` (empty: the Home composer). */
  bool IsBusy(const std::string& thread_id) const;
  /** A turn runs anywhere. */
  bool AnyBusy() const;
  /** The most thread turns at once now (MaxConcurrentTurnsFor the current configuration; 1 before there is one). */
  size_t MaxConcurrentTurns() const;
  /** Block until no session is configuring (not callable from a worker). */
  void WaitForConfigureIdle();
  /**
   * Stops every turn, waits for configures in flight, then up to `budget` until no turn is still running on the
   * turns' threads (they are not behind the runtime's teardown gate). Before what the turns use goes away (profile
   * reset); false: still running. UI thread.
   */
  bool StopTurnsAndWait(std::chrono::milliseconds budget);
  /** The thread was cleared or deleted: forget its last brief_AI role, its card and its question asked back. */
  void ForgetThread(const std::string& thread_id);
  /**
   * The Home session's conversation starts empty (AgentSession::StartNewConversation): the ask-AI panel's one-off
   * question (settings "?") goes without earlier context. False while a Home turn runs.
   */
  bool StartNewHomeConversation() { return home_->StartNewConversation(); }
  /** The ask-AI panel's one-off in the Home session: words only (AgentSession::SubmitExplanation). */
  SubmitOutcome SubmitHomeExplanation(const std::string& text) { return home_->SubmitExplanation(text); }
  /** The Home session's newest turn (AgentEvent::turn of its events). */
  uint64_t HomeTurnGeneration() const { return home_->TurnGeneration(); }

  /** The Home composer's session; its conversation is the Home transcript. */
  AgentSession& Home() { return *home_; }
  const AgentSession& Home() const { return *home_; }
  bool IsConfigured() const { return home_->IsConfigured(); }

  /** Sessions alive now, Home and the spare included. */
  size_t LiveSessions() const { return 1 + threads_.size() + (spare_ ? 1 : 0); }
  /** A configured (or configuring) spare waits for the next new thread. */
  bool HasSpare() const { return spare_ != nullptr; }
  bool HasSession(const std::string& thread_id) const { return threads_.count(thread_id) > 0; }

private:
  struct ThreadSession {
    std::unique_ptr<AgentSession> session;
    std::chrono::steady_clock::time_point last_active;
  };

  std::chrono::steady_clock::time_point Now() const;
  size_t RunningThreadTurns() const;
  std::unique_ptr<AgentSession> MakeSession();
  /** Keeps one spare once there is a configuration. */
  void WarmSpare();
  /** Calls `fn` on Home, the spare and every thread's session. */
  void ForEach(const std::function<void(AgentSession&)>& fn);
  /** The thread's session, the spare if it has none; closes an idle one first if over the soft cap. */
  AgentSession& Open(const std::string& thread_id);
  /** Runs `submit` in the thread's session if a turn may start there (Home for an empty thread). */
  SubmitOutcome SubmitIn(const std::string& thread_id, const std::function<SubmitOutcome(AgentSession&)>& submit);
  void CloseIdle();

  const std::optional<size_t> fixed_max_concurrent_turns_;
  const Clock clock_;
  std::shared_ptr<AgentSessionShared> shared_ = std::make_shared<AgentSessionShared>();
  /** Where turns run (kTurnPoolThreads); owned only here, the sessions hold it weakly. */
  std::shared_ptr<AiTurnPool> turn_pool_;

  // What a new session gets.
  std::optional<AppConfig> config_;
  ToolRegistrationHook tool_registration_hook_;
  ToolPermissionsPrefs tool_permissions_;
  ToolPermissionsSaveFn tool_permissions_saver_;
  IThreadStore* thread_store_ = nullptr;
  LocalContactsFn local_contacts_;
  BriefAiStreamFn brief_stream_;
  BriefKeyRenewFn brief_key_renewer_;

  std::unique_ptr<AgentSession> home_;
  std::unique_ptr<AgentSession> spare_;
  std::map<std::string, ThreadSession> threads_;
};

} // namespace pbr
