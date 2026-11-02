#include "feature/ai/AgentSessions.h"

#include "lib/ai/ParkedApproval.h"

#include <algorithm>
#include <utility>
#include "common/PbrCompat.h"

namespace pbr {

AgentSessions::AgentSessions(const size_t max_concurrent_turns, Clock clock)
    : max_concurrent_turns_(max_concurrent_turns), clock_(std::move(clock)) {
  redirectLogger("AgentSessions");
  home_ = MakeSession();
}

AgentSessions::~AgentSessions() = default;

std::chrono::steady_clock::time_point AgentSessions::Now() const {
  return clock_ ? clock_() : std::chrono::steady_clock::now();
}

std::unique_ptr<AgentSession> AgentSessions::MakeSession() {
  auto session = std::make_unique<AgentSession>(shared_);
  if (tool_registration_hook_) {
    session->SetToolRegistrationHook(tool_registration_hook_);
  }
  session->SetToolPermissions(tool_permissions_);
  if (tool_permissions_saver_) {
    session->SetToolPermissionsSaver(tool_permissions_saver_);
  }
  if (thread_store_) {
    session->SetThreadStore(thread_store_);
  }
  if (local_contacts_) {
    session->SetLocalContactsSource(local_contacts_);
  }
  if (brief_stream_) {
    session->SetBriefAiStream(brief_stream_);
  }
  // A question asked before this configure is done waits for it in the session (submit_when_ready).
  if (config_) {
    session->Configure(*config_);
  }
  return session;
}

// --- settings --------------------------------------------------------------------------------------

void AgentSessions::ForEach(const std::function<void(AgentSession&)>& fn) {
  fn(*home_);
  if (spare_) {
    fn(*spare_);
  }
  for (auto& [thread_id, entry] : threads_) {
    fn(*entry.session);
  }
}

void AgentSessions::WarmSpare() {
  if (!spare_ && config_) {
    spare_ = MakeSession(); // configures in the background
  }
}

void AgentSessions::Configure(const AppConfig& config) {
  config_ = config;
  ForEach([&](AgentSession& session) { session.Configure(config); });
  WarmSpare();
}

void AgentSessions::SetToolRegistrationHook(ToolRegistrationHook hook) {
  tool_registration_hook_ = std::move(hook);
  ForEach([&](AgentSession& session) { session.SetToolRegistrationHook(tool_registration_hook_); });
}

void AgentSessions::SetToolPermissions(ToolPermissionsPrefs permissions) {
  tool_permissions_ = std::move(permissions);
  ForEach([&](AgentSession& session) { session.SetToolPermissions(tool_permissions_); });
}

void AgentSessions::SetToolPermissionsSaver(ToolPermissionsSaveFn saver) {
  tool_permissions_saver_ = std::move(saver);
  ForEach([&](AgentSession& session) { session.SetToolPermissionsSaver(tool_permissions_saver_); });
}

void AgentSessions::SetThreadStore(IThreadStore* store) {
  thread_store_ = store;
  ForEach([&](AgentSession& session) { session.SetThreadStore(store); });
}

void AgentSessions::SetLocalContactsSource(LocalContactsFn source) {
  local_contacts_ = std::move(source);
  ForEach([&](AgentSession& session) { session.SetLocalContactsSource(local_contacts_); });
}

void AgentSessions::SetBriefAiStream(BriefAiStreamFn fn) {
  brief_stream_ = std::move(fn);
  ForEach([&](AgentSession& session) { session.SetBriefAiStream(brief_stream_); });
}

// --- turns -----------------------------------------------------------------------------------------

size_t AgentSessions::RunningThreadTurns() const {
  return static_cast<size_t>(std::count_if(threads_.begin(), threads_.end(),
                                            [](const auto& item) { return item.second.session->IsBusy(); }));
}

TurnStart AgentSessions::CanStart(const std::string& thread_id) const {
  if (IsBusy(thread_id)) {
    return TurnStart::BusyHere;
  }
  if (!thread_id.empty() && RunningThreadTurns() >= max_concurrent_turns_) {
    return TurnStart::Full;
  }
  return TurnStart::Yes;
}

AgentSession& AgentSessions::Open(const std::string& thread_id) {
  if (const auto it = threads_.find(thread_id); it != threads_.end()) {
    return *it->second.session;
  }
  if (LiveSessions() >= kSoftMaxLiveSessions) {
    // Over the soft cap: close the least recently used session that loses nothing by it. If every one runs a turn
    // or holds a card, open the new one anyway: the cap never makes a question wait.
    auto oldest = threads_.end();
    for (auto it = threads_.begin(); it != threads_.end(); ++it) {
      if (it->second.session->CanClose() && (oldest == threads_.end() || it->second.last_active < oldest->second.last_active)) {
        oldest = it;
      }
    }
    if (oldest != threads_.end()) {
      log().info << "closing the least recently used idle AI session (" << LiveSessions() << " live)";
      threads_.erase(oldest);
    } else {
      log().info << "every AI session is busy or holds a card: opening one more (" << LiveSessions() + 1 << " live)";
    }
  }
  ThreadSession& entry = threads_[thread_id];
  entry.session = spare_ ? std::move(spare_) : MakeSession();
  entry.last_active = Now();
  WarmSpare(); // the next new thread finds one ready too
  return *entry.session;
}

SubmitOutcome AgentSessions::SubmitIn(const std::string& thread_id,
                                      const std::function<SubmitOutcome(AgentSession&)>& submit) {
  if (thread_id.empty()) {
    return submit(*home_);
  }
  switch (CanStart(thread_id)) {
  case TurnStart::BusyHere:
    return SubmitOutcome::BusyHere;
  case TurnStart::Full:
    log().info << "AI question not started: " << RunningThreadTurns() << " thread turn(s) run, the most at once";
    return SubmitOutcome::Full;
  case TurnStart::Yes:
    break;
  }
  AgentSession& session = Open(thread_id);
  threads_[thread_id].last_active = Now();
  return submit(session);
}

SubmitOutcome AgentSessions::Submit(const std::string& user_text, std::optional<std::string> user_payload) {
  return home_->Submit(user_text, std::move(user_payload));
}

SubmitOutcome AgentSessions::SubmitToThread(const std::string& thread_id, const std::string& user_text,
                                            std::optional<std::string> user_payload,
                                            std::optional<AgentImageTurn> image) {
  if (user_text.empty()) {
    return SubmitOutcome::Ignored; // checked before a session is taken for it
  }
  return SubmitIn(thread_id, [&](AgentSession& session) {
    return session.SubmitToThread(thread_id, user_text, std::move(user_payload), std::move(image));
  });
}

SubmitOutcome AgentSessions::SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                                                std::optional<std::string> user_payload, const AtAiMode mode) {
  if (prompt.empty()) {
    return SubmitOutcome::Ignored;
  }
  return SubmitIn(thread_id, [&](AgentSession& session) {
    return session.SubmitScopedAssist(thread_id, prompt, std::move(user_payload), mode);
  });
}

SubmitOutcome AgentSessions::SubmitLocalAction(const std::string& thread_id, const std::string& user_text,
                                               const std::string& tool, Object args, const bool in_peer_chat) {
  if (!AgentSession::IsLocalActionTool(tool)) {
    log().warning << "local action payload with a tool outside the whitelist ignored: " << tool;
    return SubmitOutcome::Ignored;
  }
  return SubmitIn(thread_id, [&](AgentSession& session) {
    return session.SubmitLocalAction(thread_id, user_text, tool, std::move(args), in_peer_chat);
  });
}

Roe<void> AgentSessions::ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                              const std::string& decision_label) {
  for (auto& [thread_id, entry] : threads_) {
    if (!entry.session->HoldsApproval(approval_id)) {
      continue;
    }
    if (!entry.session->IsBusy() && RunningThreadTurns() >= max_concurrent_turns_) {
      return Error(ApprovalResumeErrorCode(ApprovalResumeError::TurnBusy));
    }
    entry.last_active = Now();
    return entry.session->ResumeToolPermission(approval_id, decision, decision_label);
  }
  // Home's card, or none: Home answers with the reason (not found, superseded, ...).
  return home_->ResumeToolPermission(approval_id, decision, decision_label);
}

bool AgentSessions::IsAppActionRequest(const std::string& text) const {
  return home_->IsAppActionRequest(text); // every session has the same configuration
}

void AgentSessions::PollEvents(std::vector<AgentEvent>& out) {
  home_->PollEvents(out); // the queue every session posts to
  CloseIdle();
}

void AgentSessions::CloseIdle() {
  const auto now = Now();
  for (auto it = threads_.begin(); it != threads_.end();) {
    ThreadSession& entry = it->second;
    if (!entry.session->CanClose()) {
      entry.last_active = now; // idle time counts from the end of what keeps it open
      ++it;
      continue;
    }
    if (now - entry.last_active >= kSessionIdleTimeout) {
      it = threads_.erase(it);
      continue;
    }
    ++it;
  }
}

void AgentSessions::Cancel(const std::string& thread_id) {
  if (thread_id.empty()) {
    home_->Cancel();
    return;
  }
  if (const auto it = threads_.find(thread_id); it != threads_.end()) {
    it->second.session->Cancel();
  }
}

void AgentSessions::CancelAll() {
  ForEach([](AgentSession& session) { session.Cancel(); });
}

bool AgentSessions::IsBusy(const std::string& thread_id) const {
  if (thread_id.empty()) {
    return home_->IsBusy();
  }
  const auto it = threads_.find(thread_id);
  return it != threads_.end() && it->second.session->IsBusy();
}

bool AgentSessions::AnyBusy() const {
  return home_->IsBusy() || RunningThreadTurns() > 0;
}

bool AgentSessions::ThreadTurnsFull() const {
  return RunningThreadTurns() >= max_concurrent_turns_;
}

void AgentSessions::WaitForConfigureIdle() {
  ForEach([](AgentSession& session) { session.WaitForConfigureIdle(); });
}

void AgentSessions::ForgetThread(const std::string& thread_id) {
  home_->ForgetThread(thread_id); // also the last-role memory every session shares
  if (const auto it = threads_.find(thread_id); it != threads_.end()) {
    it->second.session->ForgetThread(thread_id); // its card and question asked back: then it can close when idle
  }
}

} // namespace pbr
