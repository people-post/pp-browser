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
 * Thread turns that may run at the same time; the Home composer's turn is not counted. 1 keeps one turn at a time,
 * which the chat screen's queue expects until it shows several (DEV/PLAN-ai-concurrent-turns.md, steps 3-4).
 */
inline constexpr size_t kMaxConcurrentTurns = 1;
/** Live sessions (Home included) above which the least recently used idle one is closed at once. Never refuses. */
inline constexpr size_t kSoftMaxLiveSessions = 6;
/** A thread's session that could close (AgentSession::CanClose) is closed after this long without a turn. */
inline constexpr std::chrono::minutes kSessionIdleTimeout{10};

/** May a turn start in a thread now? */
enum class TurnStart {
  Yes,
  Full,     // as many thread turns run as allowed
  BusyHere, // a turn already runs in this thread
};

/**
 * The assistant's sessions: one fixed session for the Home composer (Conversation mode, no thread) and one per
 * thread, made when that thread first asks and closed once it has been idle for kSessionIdleTimeout. A thread's
 * turns always run in its own session, so its permission card, a question the app asked back and its events stay
 * with it. How many thread turns run at once is decided here and only here (`Submit*` answer Full).
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

  /** `max_concurrent_turns` and `clock` are for tests (the clock times idle sessions). */
  explicit AgentSessions(size_t max_concurrent_turns = kMaxConcurrentTurns, Clock clock = {});
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
  /** In the session holding that card; resuming starts a turn there, so it counts like one (TurnBusy when Full). */
  Roe<void> ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                 const std::string& decision_label = {});
  bool IsAppActionRequest(const std::string& text) const;
  /** The events of every session. Also closes sessions idle for kSessionIdleTimeout. Per frame. */
  void PollEvents(std::vector<AgentEvent>& out);

  /** Stops the Home composer's turn. */
  void Cancel();
  /** Stops the turn of `thread_id` only (empty: the Home composer's). */
  void Cancel(const std::string& thread_id);
  /** Stops every turn (the app pauses or quits). */
  void CancelAll();
  /** A turn runs in `thread_id` (empty: the Home composer). */
  bool IsBusy(const std::string& thread_id) const;
  /** A turn runs anywhere. */
  bool AnyBusy() const;
  /** As many thread turns run as allowed: a question in a thread without a turn would be Full. */
  bool ThreadTurnsFull() const;
  /** Block until no session is configuring (not callable from a worker). */
  void WaitForConfigureIdle();
  /** The thread was cleared or deleted: forget the brief_AI role that answered it last. */
  void ForgetThread(const std::string& thread_id);

  /** The Home composer's session; its conversation is the Home transcript. */
  AgentSession& Home() { return *home_; }
  const AgentSession& Home() const { return *home_; }
  bool IsConfigured() const { return home_->IsConfigured(); }

  /** Sessions alive now, Home included. */
  size_t LiveSessions() const { return 1 + threads_.size(); }
  bool HasSession(const std::string& thread_id) const { return threads_.count(thread_id) > 0; }

private:
  struct ThreadSession {
    std::unique_ptr<AgentSession> session;
    std::chrono::steady_clock::time_point last_active;
  };

  std::chrono::steady_clock::time_point Now() const;
  size_t RunningThreadTurns() const;
  std::unique_ptr<AgentSession> MakeSession();
  /** The thread's session, made (and configured) if it has none; closes an idle one first if over the soft cap. */
  AgentSession& Open(const std::string& thread_id);
  /** Runs `submit` in the thread's session if a turn may start there (Home for an empty thread). */
  SubmitOutcome SubmitIn(const std::string& thread_id, const std::function<SubmitOutcome(AgentSession&)>& submit);
  void CloseIdle();

  const size_t max_concurrent_turns_;
  const Clock clock_;
  std::shared_ptr<AgentSessionShared> shared_ = std::make_shared<AgentSessionShared>();

  // What a new session gets.
  std::optional<AppConfig> config_;
  ToolRegistrationHook tool_registration_hook_;
  ToolPermissionsPrefs tool_permissions_;
  ToolPermissionsSaveFn tool_permissions_saver_;
  IThreadStore* thread_store_ = nullptr;
  LocalContactsFn local_contacts_;
  BriefAiStreamFn brief_stream_;

  std::unique_ptr<AgentSession> home_;
  std::map<std::string, ThreadSession> threads_;
};

} // namespace pbr
