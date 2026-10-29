#include "feature/ai/AgentSession.h"
#include "foundation/platform/Platform.h"

#include "lib/ai/AppActionClassifier.h"
#include "lib/ai/AppActionWords.h"
#include "lib/ai/LocalActionRender.h"
#include "lib/ai/LocalizedLabels.h"
#include "lib/ai/PayloadTurnPlanBuilder.h"
#include "lib/ai/PromptBuilder.h"
#include "lib/ai/StructuredTextParser.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/ToolResultFormatter.h"
#include "lib/ai/ParkedApproval.h"
#include "lib/ai/ToolPermissionPolicy.h"
#include "lib/ai/ToolPermissionPrompt.h"
#include "lib/ai/tools/ToolRegistryBuild.h"
#include "feature/ai/TurnExecutor.h"
#include "lib/ai/TurnPlanner.h"
#include "lib/ai/conversation/Conversation.h"
#include "lib/ai/conversation/ThreadCompactionService.h"
#include "lib/ai/conversation/ThreadContextPolicy.h"
#include "lib/ai/conversation/TurnCoordinator.h"
#include "foundation/error/AppError.h"
#include "lib/ai/mcp/McpClient.h"
#include "lib/net/HttpClient.h"

#include <map>
#include "lib/ai/mcp/McpRuntime.h"
#include "foundation/data/Config.h"
#include "foundation/data/LlmPreset.h"
#include "common/Logger.h"
#include "common/Module.h"
#include "common/Utilities.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/runtime/AppVersion.h"
#include "common/Metrics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <utility>
#include "common/ValueJson.h"

#include <unordered_set>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

constexpr int kMaxIterations = 8;

/** The buttons the on-device flow renders may run these tools; edit_remark is the UI's (MessageRouter). */
bool IsDirectActionTool(const std::string& tool) {
  return tool == "open_conversation" || tool == "start_conversation" || tool == "search_people";
}

/** Identity writers of a local plan ask every time, whatever "always allow" says (plan section A). */
bool AlwaysAsksInLocalPlan(const std::string& tool) {
  return tool == "update_profile_nickname" || tool == "register_user";
}

const LocalPlanContact* FindLocalContact(const LocalPlanContext& ctx, const std::string& contact_id,
                                         const std::string& thread_id) {
  for (const LocalPlanContact& contact : ctx.contacts) {
    if (!contact_id.empty() ? contact.id == contact_id : (!thread_id.empty() && contact.thread_id == thread_id)) {
      return &contact;
    }
  }
  return nullptr;
}

/** "ai.local_action": tool, outcome and whether parameters were taken; never their content. */
void EmitLocalActionMetric(logging::Logger& log, const std::string& tool, const std::string& outcome,
                           const bool has_args) {
  log.info << "local action tool=" << (tool.empty() ? "none" : tool) << " outcome=" << outcome;
  MetricsLine("ai.local_action")
      .Add("tool", tool.empty() ? std::string("none") : tool)
      .Add("outcome", outcome)
      .Add("args", has_args ? "has" : "none")
      .Emit();
}

Value ToolCallsToJson(const std::vector<ToolCall>& tool_calls) {
  std::vector<Value> out;
  out.reserve(tool_calls.size());
  for (const ToolCall& call : tool_calls) {
    Object function;
    function.set("name", call.name);
    function.set("arguments", DumpJson(call.arguments));
    Object entry;
    entry.set("id", call.id);
    entry.set("type", "function");
    entry.set("function", function);
    out.push_back(ObjectValue(std::move(entry)));
  }
  return ArrayValue(std::move(out));
}

void AppendSynthesisReminder(std::vector<ChatMessage>& messages, const TurnPlan& plan) {
  if (messages.empty() || messages.back().role != "tool") {
    return;
  }
  messages.back().content += "\n\n" + PromptBuilder::BuildSynthesisRefinementReminder(plan);
}

} // namespace

struct AgentSession::Impl : public Module {
  Impl() { redirectLogger("AgentSession"); }

  logging::Logger& Log() const { return log(); }

  std::mutex event_mutex;
  std::vector<AgentEvent> pending_events;

  std::atomic<bool> cancelled{false};
  /** Streamed turns: a fresh cancel flag per submit, so a stopped stream can never act on a later turn. */
  std::mutex stream_cancel_mutex;
  std::shared_ptr<std::atomic<bool>> stream_cancel = std::make_shared<std::atomic<bool>>(false);
  std::atomic<uint64_t> turn_generation{0};
  /** The current turn was judged an "app action" (here or by brief_AI); FinishTurn reports whether tools ran. */
  bool handoff_turn = false;
  /** ...and it was judged on this device rather than by a handoff event. */
  bool handoff_local = false;
  std::atomic<bool> busy{false};
  std::mutex configure_mutex;
  std::condition_variable configure_cv;
  int configure_inflight = 0;
  /** Serialize ConfigureOnIO body — concurrent Configure() raced mcp.Start/Stop (UAF → SEGV in logger). */
  std::mutex configure_work_mutex;
  std::chrono::milliseconds missing_tool_wait{10'000};
  std::chrono::milliseconds missing_tool_poll{500};

  AppConfig config;
  std::unique_ptr<LlmClient> llm;
  BriefAiStreamFn brief_stream;          // built in ConfigureOnIO
  BriefAiStreamFn brief_stream_override; // SetBriefAiStream; wins over brief_stream
  McpRuntime mcp;
  ToolRegistry tools;
  Conversation conversation;
  TurnCoordinator coordinator;
  std::vector<ChatMessage> turn_scratch;
  TurnPlan turn_plan;
  TurnTrace turn_trace;
  std::optional<std::string> people_list_blocks;
  std::string pending_user_text;
  std::optional<std::string> pending_user_payload;
  std::optional<AgentImageTurn> pending_image; // at most one per turn; SubmitToThread only
  std::string pending_entry_id;
  int iterations = 0;
  bool configured = false;
  bool configure_failed = false; // the last ConfigureOnIO threw; guarded by configure_work_mutex
  bool submit_when_ready = false;
  std::chrono::steady_clock::time_point planner_started{};
  std::chrono::steady_clock::time_point synthesis_started{};

  IThreadStore* thread_store = nullptr;
  std::unique_ptr<ThreadCompactionService> compaction;
  std::string pending_thread_id;
  AgentTurnMode turn_mode = AgentTurnMode::Conversation;
  AtAiMode assist_mode = AtAiMode::Local;
  ToolRegistrationHook tool_registration_hook;
  ToolPermissionsPrefs tool_permissions;
  ToolPermissionsSaveFn tool_permissions_saver;
  std::optional<ParkedApproval> parked_approval;
  std::mutex park_mutex;

  // --- On-device app actions (DEV/PLAN-local-app-actions.md). Worker-only: turns are serialized by `busy`. ---
  /** A question the app asked back; the next message in the same AI thread answers it. */
  struct PendingLocalAction {
    LocalAsk ask;
    AgentTurnMode mode = AgentTurnMode::Conversation;
    std::string thread_id;
    int64_t expires_at_ms = 0;
    bool asked_twice = false;
  };
  std::optional<PendingLocalAction> pending_local_action;
  std::chrono::milliseconds local_ask_ttl{2 * 60 * 1000};
  LocalContactsFn local_contacts;
  /** A button's direct action for the turn SubmitLocalAction started: tool + arguments. */
  std::optional<std::pair<std::string, Object>> pending_direct_action;
  /** This turn's entries belong to an app action: stored with kLocalActionGeneration, never model history. */
  bool local_turn = false;
  /** Raw results of the executed local plan, for the template. */
  std::vector<std::string> local_raw_results;
  /** The planner context of this turn (contacts loaded only for contact tools), reused for the answer. */
  std::optional<LocalPlanContext> local_ctx;
  /** brief_AI role that answered the last completed turn of each AI thread; memory only. Guarded by `last_role_mutex`. */
  std::mutex last_role_mutex;
  std::unordered_map<std::string, std::string> last_role_by_thread;
  /** For IsAppActionRequest on the UI thread: the preset and the declared tools as of the last configure. */
  mutable std::mutex app_action_mutex;
  bool app_action_brief = false;
  std::vector<std::string> app_action_tools;
};

/** The registry's metadata for a tool; empty (never "Always", treated as read elsewhere) when unknown. */
ToolMeta AgentSession::MetaForTool(const std::shared_ptr<Impl>& state, const std::string& name) {
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    if (tool.definition.name == name) {
      return tool.meta;
    }
  }
  return {};
}

void AgentSession::PushEvent(const std::shared_ptr<Impl>& state, AgentEvent event) {
  AppRuntime::PostUI([state, event = std::move(event)]() mutable {
    std::lock_guard lock(state->event_mutex);
    state->pending_events.push_back(std::move(event));
  });
}

void AgentSession::PushLoading(const std::shared_ptr<Impl>& state, const bool loading, const std::string& thread_id) {
  // The thread lets the UI show "thinking" (and the stop key) in that conversation only, and lets it tell a late
  // "ended" of an older turn from the end of the one that runs now (B112).
  PushEvent(state, AgentEvent{.type = AgentEventType::LoadingChanged,
                              .loading = loading,
                              .thread_id = thread_id.empty() && loading ? state->pending_thread_id : thread_id});
}

bool AgentSession::IsBusy() const {
  return impl_ && impl_->busy.load();
}

void AgentSession::PushApprovalClosed(const std::shared_ptr<Impl>& state, const ParkedApproval& park,
                                      const ApprovalOutcome outcome) {
  PushEvent(state, AgentEvent{.type = AgentEventType::ApprovalClosed,
                              .entry_id = park.card_entry_id,
                              .thread_id = park.thread_id,
                              .approval_id = park.id,
                              .approval_outcome = outcome});
}

void AgentSession::PushToolActivity(const std::shared_ptr<Impl>& state, const std::string& tool_name,
                                    const std::string& status) {
  PushEvent(state, AgentEvent{.type = AgentEventType::ToolActivity, .tool_name = tool_name, .status = status});
}

void AgentSession::PushAssistantReady(const std::shared_ptr<Impl>& state, const std::string& entry_id,
                                      const std::string& text, const std::string& finish_reason,
                                      std::vector<BriefAiSource> sources, const bool from_model) {
  PushEvent(state, AgentEvent{.type = AgentEventType::AssistantReady,
                              .text = text,
                              .entry_id = entry_id,
                              .thread_id = state->pending_thread_id,
                              .finish_reason = finish_reason,
                              .scoped_assist = state->turn_mode == AgentTurnMode::ScopedAssist,
                              .shared_ai_mode = state->assist_mode,
                              .response_goal = state->turn_plan.response_goal,
                              .render_mode = state->turn_plan.render_mode,
                              .sources = std::move(sources),
                              .from_model = from_model});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const std::string& message, const bool retryable,
                             const std::string& error_kind) {
  PushEvent(state, AgentEvent{
                       .type = AgentEventType::Error, .message = message, .retryable = retryable, .error_kind = error_kind});
}

void AgentSession::PushError(const std::shared_ptr<Impl>& state, const Error& err) {
  state->Log().error << "Agent error " << AppError::Log(err);
  PushError(state, AppError::Display(err));
}

void AgentSession::FinishTurn(const std::shared_ptr<Impl>& state) {
  const std::string ended_thread = state->pending_thread_id; // cleared below; the UI matches its "ended" to the turn
  state->pending_image.reset(); // up to ~1.5 MB; not needed once the turn is over
  state->turn_trace.Log();
  if (state->handoff_turn) {
    bool parked = false;
    {
      std::lock_guard lock(state->park_mutex);
      parked = state->parked_approval && state->parked_approval->state == ParkedApprovalState::Pending;
    }
    // A handoff that ran no tool locally is the signal for a mis-routed question (plan step 5). A turn
    // parked on a permission prompt is not finished: count it when the resumed turn ends.
    if (!parked) {
      MetricsLine("ai.handoff")
          .Add("src", state->handoff_local ? "local" : "server")
          .Add("tools", static_cast<int64_t>(state->turn_trace.tools_executed.size()))
          .Emit();
      state->handoff_turn = false;
    }
  }
  state->busy = false;
  state->iterations = 0;
  state->turn_scratch.clear();
  state->people_list_blocks.reset();
  state->local_turn = false;
  state->local_raw_results.clear();
  state->local_ctx.reset();
  state->pending_direct_action.reset();
  state->pending_entry_id.clear();
  state->pending_thread_id.clear();
  state->assist_mode = AtAiMode::Local;
  state->turn_mode = AgentTurnMode::Conversation;
  PushLoading(state, false, ended_thread);
}

void AgentSession::CancelParkedApproval(const std::shared_ptr<Impl>& state, const ParkedApprovalState reason,
                                        const std::optional<std::string>& only_thread) {
  std::lock_guard lock(state->park_mutex);
  if (!state->parked_approval) {
    return;
  }
  if (only_thread && state->parked_approval->thread_id != *only_thread) {
    return;
  }
  // An Executing request was already closed (allowed / denied) when it was resumed; only a Pending one goes stale.
  const bool was_pending = state->parked_approval->state == ParkedApprovalState::Pending;
  state->parked_approval->state = reason;
  state->Log().info << "Parked approval " << state->parked_approval->id << " -> "
                    << ApprovalResumeErrorCode(reason == ParkedApprovalState::Cancelled
                                                   ? ApprovalResumeError::Superseded
                                                   : ApprovalResumeError::AlreadyResolved);
  if (was_pending) {
    PushApprovalClosed(state, *state->parked_approval, ApprovalOutcome::Stale);
  }
  state->parked_approval.reset();
}

ApprovalResumeError AgentSession::ValidateParkedResume(const std::optional<ParkedApproval>& park,
                                                       const std::string& approval_id, const bool busy) {
  if (busy) {
    return ApprovalResumeError::TurnBusy;
  }
  if (!park) {
    return ApprovalResumeError::NotFound;
  }
  if (park->id != approval_id) {
    return ApprovalResumeError::Superseded;
  }
  if (park->state == ParkedApprovalState::Resolved || park->state == ParkedApprovalState::Cancelled) {
    return ApprovalResumeError::AlreadyResolved;
  }
  if (park->state != ParkedApprovalState::Pending) {
    return ApprovalResumeError::AlreadyResolved;
  }
  constexpr int64_t kApprovalTtlMs = 30LL * 60LL * 1000LL;
  if (park->created_at_ms > 0 && util::NowUnixMs() - park->created_at_ms > kApprovalTtlMs) {
    return ApprovalResumeError::Expired;
  }
  return ApprovalResumeError::Ok;
}

void AgentSession::ParkForPermission(const std::shared_ptr<Impl>& state, TurnExecutionResult execution) {
  ParkedApproval park;
  park.id = util::GenerateUuid();
  park.thread_id = state->pending_thread_id;
  park.entry_id = state->pending_entry_id;
  park.plan = state->turn_plan;
  park.next_tool_index = execution.next_tool_index;
  park.scratch_so_far = state->turn_scratch;
  park.scratch_so_far.insert(park.scratch_so_far.end(), execution.scratch_append.begin(),
                             execution.scratch_append.end());
  park.tools_executed = execution.tools_executed;
  park.offered_tools = std::move(execution.offered_tools);
  park.state = ParkedApprovalState::Pending;
  park.created_at_ms = util::NowUnixMs();
  park.turn_mode = state->turn_mode;

  state->turn_trace.tools_executed = park.tools_executed;
  bool offer_allow_always = !park.offered_tools.empty();
  for (const PlannedToolCall& tool : park.offered_tools) {
    offer_allow_always = offer_allow_always && ToolPermissionPolicy::AllowAlwaysEligible(MetaForTool(state, tool.name));
  }
  const std::string blocks = BuildToolPermissionChoiceBlocks(park.id, park.offered_tools, offer_allow_always);

  // One slot: a card from another thread's turn that is still unanswered goes stale now (same-thread ones were
  // cancelled when this turn started).
  CancelParkedApproval(state, ParkedApprovalState::Cancelled);
  {
    std::lock_guard lock(state->park_mutex);
    state->parked_approval = std::move(park);
  }

  ValidateAndFinishAssistant(state, blocks, "stop", false, false); // FinishAssistantOutput records card_entry_id
}

void AgentSession::ResumeToolPermissionOnWorker(const std::shared_ptr<Impl>& state, std::string approval_id,
                                                std::string decision, std::string decision_label) {
  ParkedApproval park;
  {
    std::lock_guard lock(state->park_mutex);
    const ApprovalResumeError err =
        ValidateParkedResume(state->parked_approval, approval_id, state->busy.load());
    if (err != ApprovalResumeError::Ok) {
      PushError(state, std::string("Permission prompt inactive: ") + ApprovalResumeErrorCode(err));
      return;
    }
    park = *state->parked_approval;
    state->parked_approval->state = ParkedApprovalState::Executing;
  }

  if (state->busy.exchange(true)) {
    std::lock_guard lock(state->park_mutex);
    if (state->parked_approval && state->parked_approval->id == approval_id) {
      state->parked_approval->state = ParkedApprovalState::Pending;
    }
    PushError(state, std::string("Permission prompt inactive: ") +
                         ApprovalResumeErrorCode(ApprovalResumeError::TurnBusy));
    return;
  }

  state->cancelled = false;
  state->turn_mode = park.turn_mode;
  state->pending_thread_id = park.thread_id;
  state->turn_plan = park.plan;
  state->local_turn = park.plan.local;
  state->turn_scratch = park.scratch_so_far;
  state->people_list_blocks.reset();
  state->iterations = 0;
  PushLoading(state, true);

  auto append_decision_user = [&](const std::string& text) {
    if (state->turn_mode == AgentTurnMode::Conversation) {
      TranscriptEntry& entry = state->conversation.AppendUser(text, std::nullopt);
      state->pending_entry_id = entry.id;
      if (state->local_turn) {
        state->conversation.MarkLocalAction(entry.id);
      }
      return;
    }
    if (state->turn_mode == AgentTurnMode::Thread && state->thread_store) {
      ThreadMessage user_message;
      user_message.id = util::GenerateUuid();
      user_message.thread_id = state->pending_thread_id;
      user_message.sender_contact_id = kLocalSelfContactId;
      user_message.text = text;
      user_message.timestamp = util::NowUnixMs();
      user_message.delivery = MessageDelivery::Local;
      user_message.transport = MessageTransport::Local;
      if (state->local_turn) {
        user_message.generation = kLocalActionGeneration;
      }
      if (auto appended = state->thread_store->AppendMessage(user_message)) {
        state->pending_entry_id = appended->id;
      } else {
        state->pending_entry_id = user_message.id;
      }
      return;
    }
    state->pending_entry_id = util::GenerateUuid();
  };

  if (decision == "deny") {
    {
      std::lock_guard lock(state->park_mutex);
      state->parked_approval.reset();
    }
    PushApprovalClosed(state, park, ApprovalOutcome::Deny);
    append_decision_user(decision_label.empty() ? Tr("chat.permission.deny") : decision_label);
    ValidateAndFinishAssistant(state, BuildToolPermissionDeniedBlocks(park.offered_tools), "stop", false, false);
    return;
  }

  if (decision != "allow_once" && decision != "allow_always") {
    {
      std::lock_guard lock(state->park_mutex);
      if (state->parked_approval && state->parked_approval->id == approval_id) {
        state->parked_approval->state = ParkedApprovalState::Pending;
      }
    }
    FinishTurn(state);
    PushError(state, std::string("Permission prompt inactive: ") +
                         ApprovalResumeErrorCode(ApprovalResumeError::InvalidDecision));
    return;
  }

  PushApprovalClosed(state, park, decision == "allow_always" ? ApprovalOutcome::AllowAlways : ApprovalOutcome::AllowOnce);

  if (decision == "allow_always") {
    for (const PlannedToolCall& tool : park.offered_tools) {
      // The prompt only offers Always when every tool is eligible; a stale or crafted decision is not trusted.
      if (ToolPermissionPolicy::AllowAlwaysEligible(MetaForTool(state, tool.name))) {
        ToolPermissionPolicy::SetToolDecision(state->tool_permissions, tool.name, "allow");
      }
    }
    if (state->tool_permissions_saver) {
      if (auto saved = state->tool_permissions_saver(state->tool_permissions); !saved) {
        state->Log().warning << "Failed to persist tool permissions: " << saved.error().message;
      }
    }
  }

  std::unordered_set<std::string> session_grants;
  for (const PlannedToolCall& tool : park.offered_tools) {
    session_grants.insert(tool.name);
  }

  append_decision_user(decision_label.empty()
                           ? Tr(decision == "allow_always" ? "chat.permission.allow_always" : "chat.permission.allow_once")
                           : decision_label);

  const auto on_activity = [state](const std::string& tool_name, const std::string& status) {
    PushToolActivity(state, tool_name, status);
  };

  TurnExecutionOptions options;
  options.start_index = park.next_tool_index;
  options.permissions = state->tool_permissions;
  if (state->turn_plan.local) {
    // Remarks for people-list rows: only in app-action turns, whose entries never reach a model as history.
    options.local_contacts = state->local_contacts;
  }
  options.session_grants = std::move(session_grants);

  TurnExecutionResult execution = TurnExecutor::Execute(park.plan, state->tools, on_activity, options);
  {
    std::lock_guard lock(state->park_mutex);
    state->parked_approval.reset();
  }

  if (!execution.ok) {
    if (state->turn_plan.local) {
      // A tool failure is part of the answer, worded by the template; the detail only goes to the log.
      state->Log().warning << "local action failed: " << execution.error;
      FinishLocalResult(state, /*failed=*/true, execution.blocked);
      return;
    }
    PushError(state, execution.error);
    FinishTurn(state);
    return;
  }
  if (execution.needs_permission) {
    PushError(state, "Tool permission still required after approval");
    FinishTurn(state);
    return;
  }

  state->turn_trace.tools_executed = park.tools_executed;
  state->turn_trace.tools_executed.insert(state->turn_trace.tools_executed.end(), execution.tools_executed.begin(),
                                          execution.tools_executed.end());
  state->turn_scratch.insert(state->turn_scratch.end(), execution.scratch_append.begin(),
                             execution.scratch_append.end());
  state->people_list_blocks = execution.people_list_blocks;
  state->local_raw_results = std::move(execution.raw_results);
  ContinueAfterExecution(state);
}

std::vector<std::string> AgentSession::AllowedToolNames(const std::shared_ptr<Impl>& state) {
  std::vector<std::string> names;
  names.reserve(state->tools.Tools().size());
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    names.push_back(tool.definition.name);
  }
  return names;
}

void AgentSession::PopulateTurnTraceFromPlan(const std::shared_ptr<Impl>& state) {
  state->turn_trace.plan_source = state->turn_plan.source;
  state->turn_trace.response_goal = state->turn_plan.response_goal;
  state->turn_trace.render_mode = state->turn_plan.render_mode;
  state->turn_trace.tools_planned.clear();
  for (const PlannedToolCall& call : state->turn_plan.tools) {
    state->turn_trace.tools_planned.push_back(call.name);
  }
}

void AgentSession::InjectSynthesisPolicy(const std::shared_ptr<Impl>& state) {
  const std::string policy = PromptBuilder::BuildSynthesisPrompt(state->turn_plan);
  if (!state->turn_scratch.empty() && state->turn_scratch.front().role == "system") {
    if (!state->turn_scratch.front().content.empty() && !policy.empty()) {
      state->turn_scratch.front().content += "\n\n";
    }
    state->turn_scratch.front().content += policy;
    return;
  }
  state->turn_scratch.insert(state->turn_scratch.begin(), ChatMessage{.role = "system", .content = policy});
}

void AgentSession::PersistAssistantToThread(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                            std::string* out_message_id) {
  if (!state->thread_store || state->pending_thread_id.empty()) {
    return;
  }
  if (state->turn_mode == AgentTurnMode::ScopedAssist && state->assist_mode != AtAiMode::Local) {
    // Shared assist reply is persisted by the chat UI after render; do not invent a store id here.
    return;
  }

  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = state->pending_thread_id;
  message.sender_contact_id = kAiAssistantContactId;
  message.text = StructuredTextParser::StorableText(assistant_raw);
  message.timestamp = util::NowUnixMs();
  message.delivery = MessageDelivery::Local;
  message.relay_visible = state->turn_mode != AgentTurnMode::ScopedAssist;
  message.transport = MessageTransport::Local;
  if (state->local_turn) {
    message.generation = kLocalActionGeneration;
  }
  auto appended = state->thread_store->AppendMessage(message);
  if (!appended) {
    state->Log().warning << "Failed to persist assistant message: " << appended.error().message;
    return;
  }
  if (out_message_id) {
    *out_message_id = appended->id.empty() ? message.id : appended->id;
  }
}

// ParkForPermission stores the request before its card is persisted; the card's own message id is only known here.
void AgentSession::RecordParkedCardEntry(const std::shared_ptr<Impl>& state, const std::string& entry_id) {
  std::lock_guard lock(state->park_mutex);
  // Only the card's own turn records it: a shared "@ai+" card is stored by the UI (no id here), and a later answer
  // in another thread must not be taken for it.
  if (!entry_id.empty() && state->parked_approval &&
      state->parked_approval->state == ParkedApprovalState::Pending &&
      state->parked_approval->thread_id == state->pending_thread_id &&
      state->parked_approval->card_entry_id.empty()) {
    state->parked_approval->card_entry_id = entry_id;
  }
}

void AgentSession::FinishAssistantOutput(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                         const std::string& finish_reason, const bool from_model) {
  if (state->turn_mode == AgentTurnMode::Conversation) {
    state->coordinator.CompleteTurn(state->conversation, state->pending_entry_id, assistant_raw);
    if (state->local_turn) {
      state->conversation.MarkLocalAction(state->pending_entry_id);
    }
    RecordParkedCardEntry(state, state->pending_entry_id);
    PushAssistantReady(state, state->pending_entry_id, assistant_raw, finish_reason, {}, from_model);
    FinishTurn(state);
    return;
  }

  std::string assistant_message_id;
  const std::string thread_id = state->pending_thread_id;
  PersistAssistantToThread(state, assistant_raw, &assistant_message_id);
  RecordParkedCardEntry(state, assistant_message_id);
  PushAssistantReady(state, assistant_message_id, assistant_raw, finish_reason, {}, from_model);
  if (state->turn_mode == AgentTurnMode::Thread && state->compaction && !thread_id.empty()) {
    state->compaction->MaybeCompactAsync(thread_id);
  }
  FinishTurn(state);
}

void AgentSession::ValidateAndFinishAssistant(const std::shared_ptr<Impl>& state, const std::string& assistant_raw,
                                              const std::string& finish_reason, const bool allow_repair,
                                              const bool from_model) {
  const ParseResult parsed = StructuredTextParser::ParseFromLlmOutput(assistant_raw);
  state->turn_trace.parse_ok = parsed.ok;

  if (!parsed.ok && allow_repair && !state->turn_trace.output_repair_used) {
    RunOutputRepair(state, assistant_raw, parsed.error);
    return;
  }

  if (!parsed.ok) {
    state->Log().warning << "Assistant output parse failed: " << parsed.error;
  }

  FinishAssistantOutput(state, assistant_raw, finish_reason, from_model);
}

void AgentSession::RunOutputRepair(const std::shared_ptr<Impl>& state, const std::string& raw_output,
                                   const std::string& parse_error) {
  if (!state->llm) {
    FinishAssistantOutput(state, raw_output, "stop");
    return;
  }

  state->turn_trace.output_repair_used = true;
  const std::string repair_prompt =
      PromptBuilder::BuildOutputRepairPrompt(state->turn_plan, raw_output, parse_error);

  ChatCompletionRequest request;
  request.messages = state->turn_scratch;
  request.messages.push_back(ChatMessage{.role = "user", .content = repair_prompt});

  auto result = state->llm->Complete(request);
  if (!result || !result->content || result->content->empty()) {
    FinishAssistantOutput(state, raw_output, "stop");
    return;
  }

  ValidateAndFinishAssistant(state, *result->content, result->finish_reason.empty() ? "stop" : result->finish_reason,
                             false);
}

void AgentSession::DispatchRefinementToolCalls(const std::shared_ptr<Impl>& state,
                                               const std::vector<ToolCall>& tool_calls,
                                               const std::string& assistant_content) {
  state->turn_trace.refinement_used = true;

  ChatMessage assistant_message;
  assistant_message.role = "assistant";
  assistant_message.content = assistant_content;
  assistant_message.tool_calls = ToolCallsToJson(tool_calls);
  state->turn_scratch.push_back(std::move(assistant_message));

  AppRuntime::PostWorkerNormal([state, tool_calls]() {
    if (state->cancelled) {
      AgentSession::FinishTurn(state);
      return;
    }

    for (const ToolCall& call : tool_calls) {
      AgentSession::PushToolActivity(state, call.name, "running");

      ToolMeta meta;
      for (const ToolDescriptor& tool : state->tools.Tools()) {
        if (tool.definition.name == call.name) {
          meta = tool.meta;
          break;
        }
      }
      const ToolPermissionEval eval =
          ToolPermissionPolicy::Evaluate(call.name, meta, state->tool_permissions, {});
      if (eval.verdict == ToolPermissionVerdict::Ask || eval.verdict == ToolPermissionVerdict::Deny) {
        AgentSession::PushToolActivity(state, call.name, "error");
        state->turn_trace.tools_executed.push_back(call.name);
        ChatMessage tool_message;
        tool_message.role = "tool";
        tool_message.tool_call_id = call.id;
        tool_message.content =
            eval.verdict == ToolPermissionVerdict::Deny
                ? "Tool error: blocked by tool permission settings."
                : "Tool error: permission required — ask the user to confirm this action in a follow-up.";
        state->turn_scratch.push_back(std::move(tool_message));
        continue;
      }

      auto result = state->tools.Execute(call.name, call.arguments);
      AgentSession::PushToolActivity(state, call.name, result ? "done" : "error");
      state->turn_trace.tools_executed.push_back(call.name);

      ChatMessage tool_message;
      tool_message.role = "tool";
      tool_message.tool_call_id = call.id;
      if (result) {
        tool_message.content = FormatToolResultForLlm(call.name, *result);
      } else {
        tool_message.content = "Tool error: " + result.error().message;
      }
      state->turn_scratch.push_back(std::move(tool_message));
    }

    AppendSynthesisReminder(state->turn_scratch, state->turn_plan);

    ++state->iterations;
    if (state->iterations >= kMaxIterations) {
      AgentSession::PushError(state, "Agent iteration limit reached");
      AgentSession::FinishTurn(state);
      return;
    }

    AgentSession::RunSynthesisStep(state);
  });
}

void AgentSession::HandleSynthesisResponse(const std::shared_ptr<Impl>& state,
                                           const ChatCompletionResponse& response) {
  if (state->cancelled) {
    FinishTurn(state);
    return;
  }

  state->turn_trace.synthesis_ms = ElapsedMs(state->synthesis_started);

  if (!response.tool_calls.empty()) {
    DispatchRefinementToolCalls(state, response.tool_calls, response.content.value_or(""));
    return;
  }

  if (response.content) {
    if (auto embedded = StructuredTextParser::ExtractEmbeddedToolCalls(*response.content)) {
      state->Log().warning << "Synthesis returned embedded tool blocks; extracting";
      std::vector<ToolCall> tool_calls;
      tool_calls.reserve(embedded->size());
      for (size_t i = 0; i < embedded->size(); ++i) {
        tool_calls.push_back(ToolCall{
            .id = "embedded_" + std::to_string(i + 1),
            .name = (*embedded)[i].name,
            .arguments = (*embedded)[i].arguments,
        });
      }
      DispatchRefinementToolCalls(state, tool_calls, *response.content);
      return;
    }
  }

  if (!response.content || response.content->empty()) {
    PushError(state, "LLM response missing content");
    FinishTurn(state);
    return;
  }

  ValidateAndFinishAssistant(state, *response.content, response.finish_reason, true);
}

void AgentSession::RunSynthesisStep(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  state->synthesis_started = std::chrono::steady_clock::now();

  ChatCompletionRequest request;
  request.messages = state->turn_scratch;
  request.tools = state->tools.Definitions();

  auto result = state->llm->Complete(request);
  if (!result) {
    PushError(state, result.error());
    FinishTurn(state);
    return;
  }

  HandleSynthesisResponse(state, *result);
}

void AgentSession::ContinueAfterExecution(const std::shared_ptr<Impl>& state) {
  // A local plan is answered from templates, also after a parked permission was approved: never synthesis,
  // never output repair.
  if (state->turn_plan.local) {
    FinishLocalResult(state);
    return;
  }
  if (state->people_list_blocks) {
    ValidateAndFinishAssistant(state, *state->people_list_blocks, "stop", false, false);
    return;
  }
  if (ResolvePreset(state->config) == "brief") {
    // A payload turn of the brief preset whose result has no deterministic blocks: the gateway has no completions
    // route (B99), so the result is worded by the templates as well (kept in history: not an app action).
    state->Log().info << "brief preset: payload result rendered on the device, no synthesis";
    FinishLocalResult(state);
    return;
  }

  InjectSynthesisPolicy(state);
  RunSynthesisStep(state);
}

Roe<TurnPlan> AgentSession::ResolveTurnPlan(const std::shared_ptr<Impl>& state, bool& cancelled, const bool allow_model,
                                             bool& unplanned) {
  if (state->pending_user_payload && !state->pending_user_payload->empty()) {
    if (auto payload_plan = TryBuildPlanFromPayload(state->pending_user_text, *state->pending_user_payload)) {
      auto validated = ValidateTurnPlan(*payload_plan, AllowedToolNames(state));
      if (validated) {
        return validated;
      }
      if (payload_plan->response_goal == ResponseGoal::DisplayFeed) {
        // The feed tool comes from the promoted MCP server; without it there is nothing to show, and asking the
        // model instead would only produce a confusing answer. Right after launch the server may simply not have
        // been reachable yet when the session was configured, so give it a bounded chance to list its tools.
        switch (RelistMissingFeedTools(state, *payload_plan)) {
        case FeedRelist::Listed:
          if (auto relisted = ValidateTurnPlan(*payload_plan, AllowedToolNames(state))) {
            return relisted;
          }
          break;
        case FeedRelist::Cancelled:
          cancelled = true;
          return Error("Turn cancelled");
        case FeedRelist::Unavailable:
          break;
        }
        Error unavailable = validated.error();
        unavailable.user = TrOrDefault("feed.unavailable", "Briefs are unavailable right now.");
        return unavailable;
      }
    }
  }

  if (!allow_model) {
    unplanned = true;
    return Error("No plan without the model");
  }
  if (!state->llm) {
    return AppError::Config(Err::Config::MissingKey, "LLM not configured");
  }

  state->planner_started = std::chrono::steady_clock::now();
  auto plan = TurnPlanner::Plan(*state->llm, state->turn_scratch, state->tools.SummaryForPrompt(),
                                AllowedToolNames(state), state->pending_user_text);
  if (plan) {
    state->turn_trace.planner_ms = ElapsedMs(state->planner_started);
  }
  return plan;
}

AgentSession::FeedRelist AgentSession::RelistMissingFeedTools(const std::shared_ptr<Impl>& state,
                                                              const TurnPlan& plan) {
  // A short request bound: the launch case is a network that is not up yet, where one tools/list could otherwise
  // block for the transport's default 30 s while holding configure_work_mutex.
  constexpr long kRelistTimeoutS = 3;

  // Caller holds configure_work_mutex: ConfigureOnIO clears and rebuilds the registry under it.
  const auto missing = [&] {
    const std::vector<std::string> allowed = AllowedToolNames(state);
    std::string names;
    for (const PlannedToolCall& call : plan.tools) {
      if (std::find(allowed.begin(), allowed.end(), call.name) == allowed.end()) {
        names += (names.empty() ? "" : ", ") + call.name;
      }
    }
    return names;
  };

  // This turn's own flag: a new Submit after Cancel() replaces it (and clears `cancelled`), this one stays set.
  std::shared_ptr<std::atomic<bool>> cancel;
  {
    std::lock_guard lock(state->stream_cancel_mutex);
    cancel = state->stream_cancel;
  }

  const auto started = std::chrono::steady_clock::now();
  bool first = true;
  while (true) {
    if (cancel->load()) {
      state->Log().info << "Turn stopped while re-listing the promoted MCP tools";
      return FeedRelist::Cancelled;
    }
    {
      // A concurrent Configure() stops and replaces the MCP clients and rebuilds the registry.
      std::lock_guard work_lock(state->configure_work_mutex);
      const std::string wanted = missing();
      if (wanted.empty()) {
        return FeedRelist::Listed; // a Configure() that ran meanwhile listed them
      }
      McpClient* promoted = state->mcp.PromotedPtr();
      if (!promoted || !promoted->IsRunning()) {
        return FeedRelist::Unavailable; // disabled or not configured: nothing to wait for
      }
      if (promoted->IsStdio()) {
        return FeedRelist::Unavailable; // a local server ignores the short bound (30 s reads): do not wait on it
      }
      if (first) {
        state->Log().warning << "Planned tool(s) not registered: " << wanted
                             << "; re-listing the promoted MCP server for up to "
                             << state->missing_tool_wait.count() << " ms";
        first = false;
      }
      // A turn that replaced this one after Cancel() may already read the registry: register nothing then.
      const bool answered = RegisterPromotedMcpTools(state->tools, *promoted, kRelistTimeoutS, cancel.get());
      if (cancel->load()) {
        state->Log().info << "Turn stopped while re-listing the promoted MCP tools";
        return FeedRelist::Cancelled;
      }
      const std::string still = missing();
      if (still.empty()) {
        state->Log().info << "Promoted MCP tools listed after " << ElapsedMs(started) << " ms";
        return FeedRelist::Listed;
      }
      if (answered) {
        // The server is reachable (it listed, or answered with an error) and does not offer the tool: waiting
        // longer cannot help.
        state->Log().warning << "Promoted MCP server listed its tools without: " << still;
        return FeedRelist::Unavailable;
      }
    }
    if (std::chrono::steady_clock::now() - started + state->missing_tool_poll > state->missing_tool_wait) {
      break;
    }
    std::this_thread::sleep_for(state->missing_tool_poll);
  }
  state->Log().warning << "Promoted MCP server still not listing its tools after " << ElapsedMs(started) << " ms";
  return FeedRelist::Unavailable;
}

namespace {

void ResetTurnTrace(TurnTrace& trace, const std::string& entry_id, const std::string& thread_id) {
  trace = TurnTrace{};
  trace.turn_id = util::GenerateUuid();
  trace.entry_id = entry_id;
  trace.thread_id = thread_id;
}

} // namespace

void AgentSession::RunNonStreamTurn(const std::shared_ptr<Impl>& state) {
  if (ResolvePreset(state->config) == "brief") {
    // B99: the brief gateway has no completions route; a brief-preset turn never enters the model pipeline.
    state->Log().info << "brief preset: non-stream turn runs on the device (payload plan only, no model)";
    RunBriefPayloadTurn(state);
    return;
  }
  RunTurnPipeline(state);
}

void AgentSession::RunBriefPayloadTurn(const std::shared_ptr<Impl>& state) {
  ResetTurnTrace(state->turn_trace, state->pending_entry_id, state->pending_thread_id);

  const uint64_t generation = state->turn_generation.load(std::memory_order_acquire);
  bool cancelled = false;
  bool unplanned = false;
  auto plan = ResolveTurnPlan(state, cancelled, /*allow_model=*/false, unplanned);
  if (cancelled) {
    if (state->turn_generation.load(std::memory_order_acquire) == generation) {
      FinishTurn(state);
    }
    return;
  }
  if (state->pending_user_payload) {
    if (const auto doc = TryParseObject(*state->pending_user_payload);
        doc && doc->getString("type").value_or("") == "local_action") {
      // A local-flow button that reached the session without the router (no AI thread): run it directly.
      Object args;
      for (const auto& [key, value] : doc->fields()) {
        if (key != "type" && key != "tool") {
          args.set(key, value);
        }
      }
      RunDirectAction(state, doc->getString("tool").value_or(""), args);
      return;
    }
  }
  if (unplanned) {
    state->Log().warning << "brief preset: a payload without a plan; answered on the device";
    FinishLocalAnswer(state, RenderLocalUnsupported(), RenderMode::Markdown, /*keep_out_of_history=*/false);
    return;
  }
  if (!plan) {
    PushError(state, plan.error());
    FinishTurn(state);
    return;
  }
  ExecutePlan(state, std::move(*plan));
}

void AgentSession::RunTurnPipeline(const std::shared_ptr<Impl>& state) {
  if (ResolvePreset(state->config) == "brief") {
    state->Log().error << "brief preset reached the model pipeline; refused (B99)";
    RunBriefPayloadTurn(state);
    return;
  }
  ResetTurnTrace(state->turn_trace, state->pending_entry_id, state->pending_thread_id);

  const uint64_t generation = state->turn_generation.load(std::memory_order_acquire);
  bool cancelled = false;
  bool unplanned = false;
  auto plan = ResolveTurnPlan(state, cancelled, /*allow_model=*/true, unplanned);
  if (cancelled) {
    // Stopped while waiting for the feed tools: nothing to report. As with a stopped stream, only a turn that
    // was not replaced ends here; after Cancel() and a new Submit the state belongs to the new turn.
    if (state->turn_generation.load(std::memory_order_acquire) == generation) {
      FinishTurn(state);
    }
    return;
  }
  if (!plan) {
    PushError(state, plan.error());
    FinishTurn(state);
    return;
  }
  ExecutePlan(state, std::move(*plan));
}

void AgentSession::ExecutePlan(const std::shared_ptr<Impl>& state, TurnPlan plan) {
  state->turn_plan = std::move(plan);
  if (state->turn_plan.user_request.empty()) {
    state->turn_plan.user_request = state->pending_user_text;
  }
  if (state->turn_plan.local) {
    state->local_turn = true;
    MarkUserEntryLocal(state); // also when the turn parks on a permission prompt
  }
  PopulateTurnTraceFromPlan(state);

  const auto on_activity = [state](const std::string& tool_name, const std::string& status) {
    AgentSession::PushToolActivity(state, tool_name, status);
  };

  TurnExecutionOptions options;
  options.permissions = state->tool_permissions;
  if (state->turn_plan.local) {
    // Remarks for people-list rows: only in app-action turns, whose entries never reach a model as history.
    options.local_contacts = state->local_contacts;
  }
  if (state->turn_plan.local) {
    for (const PlannedToolCall& call : state->turn_plan.tools) {
      if (!AlwaysAsksInLocalPlan(call.name)) {
        continue;
      }
      // The new nickname is shown in full on the prompt; a remembered "allow" does not skip it. "Never" stays.
      const ToolPermissionEval eval =
          ToolPermissionPolicy::Evaluate(call.name, MetaForTool(state, call.name), options.permissions);
      if (eval.verdict == ToolPermissionVerdict::Allow) {
        options.permissions.by_tool[call.name] = ToolPermissionEntry{.decision = "ask"};
      }
    }
  }
  TurnExecutionResult execution = TurnExecutor::Execute(state->turn_plan, state->tools, on_activity, options);
  if (!execution.ok) {
    if (state->turn_plan.local) {
      // A tool failure is part of the answer, worded by the template; the detail only goes to the log.
      state->Log().warning << "local action failed: " << execution.error;
      FinishLocalResult(state, /*failed=*/true, execution.blocked);
      return;
    }
    PushError(state, execution.error);
    FinishTurn(state);
    return;
  }
  if (execution.needs_permission) {
    ParkForPermission(state, std::move(execution));
    return;
  }

  state->turn_trace.tools_executed = execution.tools_executed;
  state->turn_scratch.insert(state->turn_scratch.end(), execution.scratch_append.begin(), execution.scratch_append.end());
  state->people_list_blocks = execution.people_list_blocks;
  state->local_raw_results = std::move(execution.raw_results);

  ContinueAfterExecution(state);
}

bool AgentSession::UseBriefStream(const std::shared_ptr<Impl>& state) {
  // A scoped assist ("@ai" in a chat) decides for itself in StartTurn: only its local mode streams.
  return state->turn_mode != AgentTurnMode::ScopedAssist && BriefStreamAvailable(state);
}

bool AgentSession::BriefStreamAvailable(const std::shared_ptr<Impl>& state) {
  if (state->pending_user_payload && !state->pending_user_payload->empty()) {
    return false;
  }
  if (!state->brief_stream && !state->brief_stream_override) {
    return false;
  }
  return ResolvePreset(state->config) == "brief";
}

namespace {

/** A settings-read tool: "我的设置是什么" is a request for it, not a how-to question. */
bool ToolIsSettingsRead(const std::string& tool) {
  for (const AppActionWords& words : AppActionTable()) {
    if (words.tool == tool) {
      return words.settings_read;
    }
  }
  return false;
}

} // namespace

std::vector<std::string> AgentSession::AppActionCapabilities(const std::shared_ptr<Impl>& state) {
  std::vector<std::string> names;
  for (const ToolDescriptor& tool : state->tools.Tools()) {
    if (tool.meta.provider == "messaging" || tool.meta.provider == "settings") {
      names.push_back(tool.definition.name);
    }
  }
  return names;
}

bool AgentSession::TryLocalAppAction(const std::shared_ptr<Impl>& state) {
  if (state->pending_image) {
    return false;
  }
  if (IsAppOverviewQuestion(state->pending_user_text)) {
    // "What can you do?": the device lists its own commands; the cloud model does not know them.
    EmitLocalActionMetric(state->Log(), "overview", "question", false);
    // Static text without user data: it stays in the history, so a follow-up can refer to it. Not a handoff.
    FinishLocalAnswer(state, RenderLocalOverview(), RenderMode::Markdown, /*keep_out_of_history=*/false);
    return true;
  }
  const auto match = ClassifyAppActionMatch(state->pending_user_text, AppActionCapabilities(state));
  if (!match) {
    return false;
  }
  LocalPlan plan = BuildLocalTurnPlan(*match, state->pending_user_text, BuildLocalPlanContext(state, match->tool));
  if (std::holds_alternative<LocalQuestion>(plan)) {
    EmitLocalActionMetric(state->Log(), match->tool, "question", false);
    if (HasLocalHowTo(match->tool)) {
      // "怎么搜索用户？" asks how to use the app: a short how-to from the device, no stream, no model.
      FinishLocalAnswer(state, RenderLocalHowTo(match->tool), RenderMode::Markdown, /*keep_out_of_history=*/false);
      return true;
    }
    return false; // other questions about the app: brief_AI answers them
  }
  state->Log().info << "Turn judged an app action on the device (" << match->tool << "); answering on the device";
  state->handoff_turn = true;
  state->handoff_local = true;
  HandleLocalPlan(state, std::move(plan), match->tool, "executed");
  return true;
}

bool AgentSession::TryPendingLocalAnswer(const std::shared_ptr<Impl>& state) {
  if (!state->pending_local_action || state->pending_image) {
    return false;
  }
  const Impl::PendingLocalAction& pending = *state->pending_local_action;
  const bool same_thread = pending.mode == state->turn_mode && pending.thread_id == state->pending_thread_id &&
                           (state->turn_mode != AgentTurnMode::ScopedAssist || state->assist_mode == AtAiMode::Local);
  if (!same_thread) {
    return false; // a message elsewhere leaves the question open
  }
  const Impl::PendingLocalAction taken = *std::exchange(state->pending_local_action, std::nullopt);
  if (util::NowUnixMs() > taken.expires_at_ms) {
    state->Log().info << "local action question expired (" << taken.ask.tool << "); routing the message normally";
    return false;
  }

  const std::string& text = state->pending_user_text;
  if (taken.ask.missing == "query" || taken.ask.missing == "nickname") {
    // Any sentence can be a name ("搜索达人": a verb alone does not classify), but a full command — verb and
    // object, or a whole phrase of the wordlist, like "关掉通知" — is not an answer: the question is dropped and
    // the sentence routed like any other.
    if (IsAppOverviewQuestion(text) || ClassifyAppActionMatch(text, AppActionCapabilities(state))) {
      state->Log().info << "local action question dropped for a new command (" << taken.ask.tool << ")";
      return false;
    }
  }
  LocalPlan plan = ResolvePendingAnswer(taken.ask, text, BuildLocalPlanContext(state, taken.ask.tool));
  if (std::holds_alternative<LocalQuestion>(plan)) {
    EmitLocalActionMetric(state->Log(), taken.ask.tool, "question", false);
    return false;
  }
  if (std::holds_alternative<LocalAsk>(plan)) {
    if (IsAppOverviewQuestion(text) || ClassifyAppActionMatch(text, AppActionCapabilities(state))) {
      // Not an answer but a new command: the question is dropped and the message routed like any other.
      state->Log().info << "local action question dropped for a new command (" << taken.ask.tool << ")";
      return false;
    }
    if (taken.asked_twice) {
      EmitLocalActionMetric(state->Log(), taken.ask.tool, "gave_up", false);
      state->handoff_turn = true;
      state->handoff_local = true;
      FinishLocalAnswer(state, RenderLocalGaveUp(), RenderMode::Markdown);
      return true;
    }
  }
  state->handoff_turn = true;
  state->handoff_local = true;
  HandleLocalPlan(state, std::move(plan), taken.ask.tool, "followup", /*asked_twice=*/true);
  return true;
}

void AgentSession::HandleHandoff(const std::shared_ptr<Impl>& state, const std::string& capability) {
  const std::vector<std::string> declared = AppActionCapabilities(state);
  if (capability.empty() || std::find(declared.begin(), declared.end(), capability) == declared.end()) {
    state->Log().info << "brief_AI handed the turn back for an unknown capability; answered on the device";
    EmitLocalActionMetric(state->Log(), capability, "unsupported", false);
    FinishLocalAnswer(state, RenderLocalUnsupported(), RenderMode::Markdown);
    return;
  }
  // A handed-back turn never goes back to the stream: a question, or a sentence the wordlist does not read as
  // this tool, gets the tool's question (no parameters taken from it).
  LocalPlan plan;
  const auto match = ClassifyAppActionMatch(state->pending_user_text, declared);
  if (match && match->tool == capability) {
    plan = BuildLocalTurnPlan(*match, state->pending_user_text, BuildLocalPlanContext(state, capability));
    if (std::holds_alternative<LocalQuestion>(plan)) {
      plan = AskForTool(capability, state->pending_user_text);
    }
  } else {
    plan = AskForTool(capability, state->pending_user_text);
  }
  if (!std::holds_alternative<TurnPlan>(plan) && IsLocalActionQuestion(state->pending_user_text, ToolIsSettingsRead(capability))) {
    // "怎么搜索用户？": a how-to question, not a request; "要搜谁？" would be a silly answer.
    state->local_turn = true;
    EmitLocalActionMetric(state->Log(), capability, "question", false);
    FinishLocalAnswer(state, RenderLocalHowTo(capability), RenderMode::Markdown);
    return;
  }
  HandleLocalPlan(state, std::move(plan), capability, "executed");
}

void AgentSession::HandleLocalPlan(const std::shared_ptr<Impl>& state, LocalPlan plan, const std::string& tool,
                                   const char* outcome, const bool asked_twice) {
  state->local_turn = true;
  if (auto* turn = std::get_if<TurnPlan>(&plan)) {
    turn->local = true;
    const std::string planned = turn->tools.empty() ? tool : turn->tools.front().name;
    EmitLocalActionMetric(state->Log(), planned, outcome, !turn->tools.empty() && !turn->tools.front().arguments.empty());
    ExecutePlan(state, std::move(*turn));
    return;
  }
  if (const auto* ask = std::get_if<LocalAsk>(&plan)) {
    EmitLocalActionMetric(state->Log(), ask->tool, "asked", false);
    // Recorded before the turn ends: FinishTurn clears the thread id.
    state->pending_local_action = Impl::PendingLocalAction{
        .ask = *ask,
        .mode = state->turn_mode,
        .thread_id = state->pending_thread_id,
        .expires_at_ms = util::NowUnixMs() + static_cast<int64_t>(state->local_ask_ttl.count()),
        .asked_twice = asked_twice,
    };
    FinishLocalAnswer(state, RenderLocalAsk(*ask), RenderMode::Markdown);
    return;
  }
  if (const auto* ambiguous = std::get_if<LocalAmbiguous>(&plan)) {
    EmitLocalActionMetric(state->Log(), ambiguous->tool, "ambiguous", false);
    FinishLocalAnswer(state, RenderLocalAmbiguous(*ambiguous, ambiguous->query), RenderMode::PeopleList);
    return;
  }
  if (std::holds_alternative<LocalCancelled>(plan)) {
    EmitLocalActionMetric(state->Log(), tool, "cancelled", false);
    FinishLocalAnswer(state, RenderLocalCancelled(), RenderMode::Markdown);
    return;
  }
  // LocalUnsupported (and LocalQuestion, which callers resolve before this point).
  EmitLocalActionMetric(state->Log(), tool, "unsupported", false);
  FinishLocalAnswer(state, RenderLocalUnsupported(), RenderMode::Markdown);
}

void AgentSession::RunDirectAction(const std::shared_ptr<Impl>& state, const std::string& tool, const Object& args) {
  ResetTurnTrace(state->turn_trace, state->pending_entry_id, state->pending_thread_id);
  state->local_turn = true;
  if (!IsDirectActionTool(tool)) {
    state->Log().warning << "local action payload with a tool outside the whitelist ignored: " << tool;
    FinishTurn(state);
    return;
  }
  const LocalPlanContext ctx = BuildLocalPlanContext(state, tool);
  TurnPlan plan;
  plan.source = TurnPlanSource::Payload;
  plan.local = true;
  plan.user_request = state->pending_user_text;
  std::string run = tool;
  Object run_args;
  if (tool == "search_people") {
    plan.response_goal = ResponseGoal::PeopleDiscovery;
    plan.render_mode = RenderMode::PeopleList;
    run_args.set("query", args.getString("query").value_or(""));
  } else {
    // A row of an older list may name a contact that was deleted since.
    const LocalPlanContact* contact =
        FindLocalContact(ctx, args.getString("contact_id").value_or(""), args.getString("thread_id").value_or(""));
    if (!contact) {
      EmitLocalActionMetric(state->Log(), tool, "gone", false);
      FinishLocalAnswer(state, RenderLocalContactGone(), RenderMode::Markdown);
      return;
    }
    if (tool == "open_conversation" && !contact->thread_id.empty()) {
      run_args.set("thread_id", contact->thread_id);
    } else {
      run = "start_conversation"; // no chat with this contact yet: starting one opens it
      run_args.set("contact_id", contact->id);
    }
  }
  plan.tools.push_back(PlannedToolCall{run, std::move(run_args)});
  EmitLocalActionMetric(state->Log(), run, "picked", true);
  ExecutePlan(state, std::move(plan));
}

LocalPlanContext AgentSession::BuildLocalPlanContext(const std::shared_ptr<Impl>& state, const std::string& tool) {
  const bool contact_tool = tool == "open_conversation" || tool == "start_conversation";
  if (contact_tool && state->local_ctx) {
    return *state->local_ctx; // loaded once per turn: the plan and its answer read the same contacts
  }
  LocalPlanContext ctx;
  if (contact_tool && state->local_contacts) {
    // Contacts (and their threads) are read only for the tools that resolve a name.
    ctx.contacts = state->local_contacts();
  }
  if (tool == "set_language") {
    // The shipped locales, as the settings tool lists them (no I/O).
    if (auto listed = state->tools.Execute("list_locales", Object{})) {
      if (const auto doc = TryParseObject(*listed)) {
        if (const Array* locales = doc->getArray("locales")) {
          for (const Value& entry : locales->elements) {
            if (const Object* locale = asObject(entry)) {
              ctx.locales.emplace_back(locale->getString("tag").value_or(""), locale->getString("label").value_or(""));
            }
          }
        }
      }
    }
  }
  if (contact_tool) {
    state->local_ctx = ctx;
  }
  return ctx;
}

void AgentSession::MarkUserEntryLocal(const std::shared_ptr<Impl>& state) {
  if (state->turn_mode == AgentTurnMode::Conversation) {
    state->conversation.MarkLocalAction(state->pending_entry_id);
    return;
  }
  if (state->turn_mode != AgentTurnMode::Thread || !state->thread_store || state->pending_entry_id.empty()) {
    return; // "@ai" in a chat: the question is not part of any model history
  }
  auto stored = state->thread_store->GetMessage(state->pending_thread_id, state->pending_entry_id);
  if (!stored || !*stored || IsLocalActionMessage(**stored)) {
    return;
  }
  ThreadMessage message = **stored;
  message.generation = kLocalActionGeneration;
  if (auto updated = state->thread_store->UpdateMessage(message); !updated) {
    state->Log().warning << "Failed to mark the app-action question: " << updated.error().message;
  }
}

void AgentSession::FinishLocalAnswer(const std::shared_ptr<Impl>& state, const std::string& text,
                                     const RenderMode mode, const bool keep_out_of_history) {
  state->local_turn = keep_out_of_history;
  if (keep_out_of_history) {
    MarkUserEntryLocal(state);
  }
  // The event carries the plan's render mode: a host-built answer is Markdown text or people-list blocks.
  const bool local_plan = state->turn_plan.local;
  state->turn_plan = TurnPlan{};
  state->turn_plan.local = local_plan;
  state->turn_plan.render_mode = mode;
  state->turn_plan.response_goal =
      mode == RenderMode::PeopleList ? ResponseGoal::PeopleDiscovery : ResponseGoal::General;
  FinishAssistantOutput(state, text, "stop", /*from_model=*/false);
}

void AgentSession::FinishLocalResult(const std::shared_ptr<Impl>& state, const bool failed, const bool blocked) {
  const bool keep_out = state->turn_plan.local;
  const PlannedToolCall call = state->turn_plan.tools.empty() ? PlannedToolCall{} : state->turn_plan.tools.back();
  if (!failed && state->people_list_blocks) {
    FinishLocalAnswer(state, *state->people_list_blocks, RenderMode::PeopleList, keep_out);
    return;
  }
  Object result;
  if (failed) {
    result.set("success", false);
    result.set("blocked", blocked);
  } else if (!state->local_raw_results.empty()) {
    if (const auto doc = TryParseObject(state->local_raw_results.back())) {
      result = *doc;
    }
  }
  LocalRenderContext ctx;
  ctx.args = call.arguments;
  ctx.synthesis_hints = state->turn_plan.synthesis_hints;
  if (call.name == "open_conversation" || call.name == "start_conversation") {
    const LocalPlanContext contacts = BuildLocalPlanContext(state, call.name);
    if (const LocalPlanContact* contact = FindLocalContact(contacts, call.arguments.getString("contact_id").value_or(""),
                                                           call.arguments.getString("thread_id").value_or(""))) {
      ctx.name = !contact->remark.empty() ? contact->remark : contact->nickname; // UI text only
    }
  }
  std::string text = RenderLocalResult(call.name, result, ctx);
  if (text.empty()) {
    text = Tr("ai.done.generic"); // list_conversations: no template of its own yet
  }
  FinishLocalAnswer(state, text, RenderMode::Markdown, keep_out);
}

void AgentSession::EmitStreamedAnswer(const std::shared_ptr<Impl>& state, const std::string& text,
                                      const std::string& finish_reason, const std::vector<BriefAiSource>& sources) {
  state->turn_plan = TurnPlan{};
  state->turn_plan.render_mode = RenderMode::Markdown;

  if (state->turn_mode == AgentTurnMode::Conversation) {
    state->coordinator.CompleteTurn(state->conversation, state->pending_entry_id, text);
    PushAssistantReady(state, state->pending_entry_id, text, finish_reason, sources);
    return;
  }

  std::string assistant_message_id;
  const std::string thread_id = state->pending_thread_id;
  PersistAssistantToThread(state, text, &assistant_message_id);
  PushAssistantReady(state, assistant_message_id, text, finish_reason, sources);
  if (state->turn_mode == AgentTurnMode::Thread && state->compaction && !thread_id.empty()) {
    state->compaction->MaybeCompactAsync(thread_id);
  }
}

void AgentSession::StreamBriefTurn(const std::shared_ptr<Impl>& state, std::vector<BriefAiHistoryTurn> history,
                                   std::string summary) {
  // Same gate as LlmClient::Complete: no key means a settings problem, not a network one — say so
  // without a round-trip and without offering a retry.
  if (state->config.llm.api_key.empty()) {
    PushError(state, AppError::Config(Err::Config::MissingKey, "LLM API key not configured"));
    FinishTurn(state);
    return;
  }
  state->turn_trace = TurnTrace{};
  state->turn_trace.turn_id = util::GenerateUuid();
  state->turn_trace.entry_id = state->pending_entry_id;
  state->turn_trace.thread_id = state->pending_thread_id;

  BriefAiRequest request;
  request.message = state->pending_user_text;
  request.history = std::move(history); // the client keeps the last 6
  request.summary = std::move(summary);
  // The role that answered this chat's previous AI turn: an AI thread, or "@ai" in a person chat (keyed by that chat;
  // brief_AI routes "@ai" questions by content like any other). The home composer has no chat to remember it for.
  const std::string role_thread_id =
      state->turn_mode == AgentTurnMode::Conversation ? std::string() : state->pending_thread_id;
  if (!role_thread_id.empty()) {
    std::lock_guard lock(state->last_role_mutex);
    if (const auto it = state->last_role_by_thread.find(role_thread_id); it != state->last_role_by_thread.end()) {
      request.last_role = it->second;
    }
  }
  request.app_version = AppVersionString();
  if (state->pending_image) {
    request.image = state->pending_image->image;
  }
  // A turn with an image is never handed back, so it advertises no capabilities; neither does "@ai" in a
  // chat (contract: only the question goes out).
  if (!request.image && state->turn_mode != AgentTurnMode::ScopedAssist) {
    request.capabilities = AppActionCapabilities(state);
  }

  // This turn's cancel flag and generation. Cancel() + a new Submit replace both; a stream that is
  // still draining then sees a stale generation and must not touch the new turn's state.
  std::shared_ptr<std::atomic<bool>> cancel;
  {
    std::lock_guard lock(state->stream_cancel_mutex);
    cancel = state->stream_cancel;
  }
  const uint64_t generation = state->turn_generation.load(std::memory_order_acquire);
  const auto current = [state, generation]() {
    return state->turn_generation.load(std::memory_order_acquire) == generation;
  };

  // Operational metrics only: timings and outcome, never text.
  struct TurnStats {
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    std::optional<std::chrono::steady_clock::time_point> first_token;
    size_t deltas = 0;
    std::string route;
  };
  auto stats = std::make_shared<TurnStats>();
  // Size bucket only: no name, dimensions or content.
  const bool has_image = request.image.has_value();
  const char* image_kb = !has_image ? "none" : request.image->data.size() < 500u * 1024u ? "<500"
                         : request.image->data.size() < 1536u * 1024u              ? "<1536"
                                                                                   : ">=1536";
  const auto emit_turn = [stats, has_image, image_kb](const char* outcome) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) -> int64_t {
      return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count());
    };
    MetricsLine("ai.turn")
        .Add("outcome", outcome)
        .Add("route", stats->route.empty() ? "none" : stats->route)
        .Add("first_token_ms", stats->first_token ? ms(stats->started, *stats->first_token) : int64_t{-1})
        .Add("total_ms", ms(stats->started, now))
        .Add("deltas", static_cast<int64_t>(stats->deltas))
        .Add("image", static_cast<int64_t>(has_image ? 1 : 0))
        .Add("image_kb", image_kb)
        .Emit();
  };

  // Image turn only: which HTTP status (413 / 400) ended the request, so the GUI can word it itself.
  auto image_error_kind = std::make_shared<std::string>();

  BriefTurnSinks sinks;
  // Deltas carry the whole text so far; cap them at 20/s so a long answer does not flood the UI queue.
  auto last_delta = std::make_shared<std::optional<std::chrono::steady_clock::time_point>>();
  sinks.on_delta = [state, current, last_delta, stats](const std::string& text) {
    const auto now = std::chrono::steady_clock::now();
    if (!stats->first_token) {
      stats->first_token = now;
    }
    ++stats->deltas;
    if (!current() || (*last_delta && now - **last_delta < std::chrono::milliseconds(50))) {
      return;
    }
    *last_delta = now;
    PushEvent(state, AgentEvent{.type = AgentEventType::AssistantDelta,
                                .text = text,
                                .entry_id = state->pending_entry_id,
                                .thread_id = state->pending_thread_id});
  };
  sinks.on_status = [state, current](const std::string& tool, const std::string& phase, const std::string& /*query*/) {
    if (current()) {
      PushToolActivity(state, tool, phase);
    }
  };
  sinks.on_meta = [stats](const std::string& route) { stats->route = route; };
  sinks.on_done = [state, current, emit_turn, role_thread_id](const std::string& response, const std::string& finish,
                                                              const std::vector<BriefAiSource>& sources,
                                                              const std::string& role) {
    emit_turn(finish == "length" ? "done_truncated" : "done");
    if (!current()) {
      state->Log().info << "brief_AI stream finished after the turn was replaced; dropped";
      return;
    }
    if (!role_thread_id.empty()) {
      std::lock_guard lock(state->last_role_mutex);
      if (role.empty()) {
        state->last_role_by_thread.erase(role_thread_id); // the last answer named no role: nothing to hand back
      } else {
        state->last_role_by_thread[role_thread_id] = role;
      }
    }
    EmitStreamedAnswer(state, response, finish, sources);
    FinishTurn(state);
  };
  sinks.on_handoff = [state, current, emit_turn, has_image](const std::string& capability) {
    emit_turn("handoff");
    if (!current()) {
      return;
    }
    if (has_image) {
      // The contract never hands back a turn with an image; do not run the local pipeline on one.
      state->Log().warning << "brief_AI handed back a turn with an image; dropped";
      PushError(state, "The AI could not answer this image. Try again.");
      FinishTurn(state);
      return;
    }
    state->Log().info << "brief_AI handed the turn back (" << (capability.empty() ? "none" : capability)
                      << "); answering on the device";
    state->handoff_turn = true;
    state->handoff_local = false;
    HandleHandoff(state, capability);
  };
  sinks.on_error = [state, current, emit_turn, image_error_kind](const std::string& message, const bool retryable,
                                                                 const std::string& partial) {
    state->Log().warning << "brief_AI stream error (retryable=" << retryable << ", partial_chars=" << partial.size() << ")";
    emit_turn(partial.empty() ? "error" : "error_partial");
    if (!current()) {
      return;
    }
    if (!partial.empty()) {
      EmitStreamedAnswer(state, partial, "error", {});
    }
    PushError(state, message, retryable, *image_error_kind);
    FinishTurn(state);
  };
  sinks.on_cancelled = [state, current, emit_turn](const std::string& partial) {
    emit_turn("cancelled");
    if (!current()) {
      return; // Cancel() followed by a new Submit: the new turn owns the state now
    }
    if (!partial.empty()) {
      EmitStreamedAnswer(state, partial, "cancelled", {});
    }
    FinishTurn(state);
  };

  BriefAiStreamFn stream = state->brief_stream_override ? state->brief_stream_override : state->brief_stream;
  if (has_image) {
    // The failed status is only in the technical detail ("LLM HTTP 413: ..."); note it for the error sink.
    stream = [inner = std::move(stream), image_error_kind](const BriefAiRequest& req,
                                                           const std::function<void(const BriefAiEvent&)>& on_event,
                                                           const std::atomic<bool>& cancelled) -> Roe<BriefAiOutcome> {
      Roe<BriefAiOutcome> outcome = inner(req, on_event, cancelled);
      if (!outcome) {
        const std::string& detail = outcome.error().message;
        if (detail.find("LLM HTTP 413") != std::string::npos) {
          *image_error_kind = "image_too_large";
        } else if (detail.find("LLM HTTP 400") != std::string::npos) {
          *image_error_kind = "image_unsupported";
        }
      }
      return outcome;
    };
  }
  BriefTurnRunner::Run(stream, request, *cancel, sinks);
}

void AgentSession::StartTurn(const std::shared_ptr<Impl>& state) {
  if (state->cancelled || !state->llm) {
    FinishTurn(state);
    return;
  }

  // A request still waiting for the user is given up when the conversation moves on in the SAME thread; a turn in
  // another thread (e.g. a queued question) leaves it alone.
  CancelParkedApproval(state, ParkedApprovalState::Cancelled, state->pending_thread_id);

  state->iterations = 0;
  state->turn_scratch.clear();
  state->people_list_blocks.reset();
  PushLoading(state, true);

  if (auto action = std::exchange(state->pending_direct_action, std::nullopt)) {
    // A button of the on-device flow: the label is the user's turn (an app-action entry), then the action runs.
    state->local_turn = true;
    if (!IsDirectActionTool(action->first)) {
      RunDirectAction(state, action->first, action->second); // logs and ends the turn
      return;
    }
    if (state->turn_mode == AgentTurnMode::Conversation) {
      TranscriptEntry& entry = state->conversation.AppendUser(state->pending_user_text, std::nullopt);
      state->pending_entry_id = entry.id;
      state->conversation.MarkLocalAction(entry.id);
    } else if (state->turn_mode == AgentTurnMode::Thread && state->thread_store) {
      ThreadMessage user_message;
      user_message.id = util::GenerateUuid();
      user_message.thread_id = state->pending_thread_id;
      user_message.sender_contact_id = kLocalSelfContactId;
      user_message.text = state->pending_user_text;
      user_message.timestamp = util::NowUnixMs();
      user_message.delivery = MessageDelivery::Local;
      user_message.transport = MessageTransport::Local;
      user_message.generation = kLocalActionGeneration;
      auto appended = state->thread_store->AppendMessage(user_message);
      state->pending_entry_id = appended ? appended->id : user_message.id;
    } else {
      state->pending_entry_id = util::GenerateUuid();
    }
    RunDirectAction(state, action->first, action->second);
    return;
  }

  if (state->turn_mode == AgentTurnMode::Thread) {
    if (!state->thread_store) {
      PushError(state, "Thread store not configured");
      FinishTurn(state);
      return;
    }

    if (state->pending_image && !UseBriefStream(state)) {
      // Only the streamed brief path carries an image; never drop it silently.
      PushError(state, "Images can only be sent with the Brief AI model.");
      FinishTurn(state);
      return;
    }

    ThreadMessage user_message;
    user_message.id = state->pending_image && !state->pending_image->message_id.empty()
                          ? state->pending_image->message_id
                          : util::GenerateUuid();
    user_message.thread_id = state->pending_thread_id;
    user_message.sender_contact_id = kLocalSelfContactId;
    // Stored as text: later history reads "[Image] <question>"; the bytes are never kept here. The GUI's
    // reference to the stored image (private CAS) rides along so the bubble can show it after a restart.
    user_message.text = (state->pending_image ? std::string(kAiImageTurnMarker) : std::string()) + state->pending_user_text;
    if (state->pending_image && !state->pending_image->ai_image_json.empty()) {
      user_message.ai_image_json = state->pending_image->ai_image_json;
    }
    user_message.timestamp = util::NowUnixMs();
    user_message.delivery = MessageDelivery::Local;
    user_message.transport = MessageTransport::Local;
    if (auto appended = state->thread_store->AppendMessage(user_message)) {
      state->pending_entry_id = appended->id;
    } else {
      state->pending_entry_id = user_message.id;
    }

    auto messages = state->thread_store->GetMessagesForContext(state->pending_thread_id, state->config.context);
    if (!messages) {
      PushError(state, messages.error());
      FinishTurn(state);
      return;
    }

    std::optional<ConversationSummary> summary;
    if (auto memory = state->thread_store->GetThreadMemory(state->pending_thread_id)) {
      summary = *memory;
    }

    ThreadContextPolicy policy(state->config.context);
    const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
    const ContextBuildResult built = policy.Build(*messages, system_prompt, state->pending_user_text,
                                                  state->pending_user_payload, summary);
    state->turn_scratch = built.messages;
    if (UseBriefStream(state)) {
      if (TryPendingLocalAnswer(state) || TryLocalAppAction(state)) {
        return;
      }
      // Built context is a transcript blob plus a pending user turn; send the real messages instead.
      std::vector<BriefAiHistoryTurn> history;
      for (const ThreadMessage& message : *messages) {
        if (message.id == state->pending_entry_id || message.text.empty() || IsLocalActionMessage(message)) {
          continue; // app-action entries may carry contact remarks: never sent to a model
        }
        const bool assistant = message.sender_contact_id == kAiAssistantContactId;
        // Local-pipeline answers are stored as UI block documents; the backend gets their prose, and a
        // document with nothing readable (buttons, a form) is left out rather than sent as JSON.
        std::string content = message.text;
        if (assistant) {
          if (const auto prose = StructuredTextParser::PlainTextIfBlocks(message.text)) {
            if (prose->empty()) {
              continue;
            }
            content = *prose;
          }
        }
        history.push_back(BriefAiHistoryTurn{.role = assistant ? "assistant" : "user", .content = std::move(content)});
      }
      StreamBriefTurn(state, std::move(history), summary ? summary->text : std::string());
      return;
    }
    RunNonStreamTurn(state);
    return;
  }

  if (state->turn_mode == AgentTurnMode::ScopedAssist) {
    if (!state->thread_store) {
      PushError(state, "Thread store not configured");
      FinishTurn(state);
      return;
    }

    state->pending_entry_id = util::GenerateUuid();

    // Only the question goes out; the chat transcript stays on the device (plan decision 11).
    ThreadContextPolicy policy(state->config.context);
    state->turn_scratch = policy.BuildAssistContext(state->pending_user_text);
    if (!state->turn_scratch.empty() && state->turn_scratch.front().role == "system") {
      state->turn_scratch.front().content =
          PromptBuilder::BuildScopedAssistSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
    }
    // With the brief preset an "@ai" in a chat is a brief_AI question like any other, so it gets the
    // current date, retrieval and sources (the bare model answered from its training data: wrong year,
    // wrong language). Only the question goes out, in every mode: "@ai+" / "@ai++" differ from "@ai" in
    // what the chat UI relays to the other side after rendering, not in what the AI receives. The local
    // pipeline (OpenAI-style completions) is only for other presets: the brief gateway has no completions
    // route any more (B99), so a shared reply that kept the local path answered with HTTP 404.
    const bool stream = BriefStreamAvailable(state);
    state->Log().info << "scoped assist path=" << (stream ? "brief_stream" : "pipeline")
                      << " preset=" << ResolvePreset(state->config) << " key=" << (state->config.llm.api_key.empty() ? "none" : "set");
    if (state->assist_mode != AtAiMode::Local && ResolvePreset(state->config) == "brief") {
      // "@ai++ 搜索用户 ken": an app action is not shared with the other side. Decided before anything else
      // (stream available or not): nothing is executed, asked or relayed; the answer stays on this device.
      if (const auto match = ClassifyAppActionMatch(state->pending_user_text, AppActionCapabilities(state))) {
        EmitLocalActionMetric(state->Log(), match->tool, "not_shareable", false);
        state->assist_mode = AtAiMode::Local;
        FinishLocalAnswer(state, RenderLocalNotShareable(), RenderMode::Markdown);
        return;
      }
    }
    if (stream) {
      // "@ai 搜索用户 ken" in a chat runs on the device like in an AI thread; the answer stays in this chat.
      if (state->assist_mode == AtAiMode::Local && (TryPendingLocalAnswer(state) || TryLocalAppAction(state))) {
        return;
      }
      StreamBriefTurn(state, {}, std::string());
      return;
    }
    RunNonStreamTurn(state);
    return;
  }

  TranscriptEntry& entry = state->conversation.AppendUser(state->pending_user_text, state->pending_user_payload);
  state->pending_entry_id = entry.id;

  const std::string system_prompt = PromptBuilder::BuildChatAgentSystemPrompt(state->tools.SummaryForPrompt(), ResolvePreset(state->config) == "brief");
  const TurnSnapshot snapshot =
      state->coordinator.BeginTurn(state->conversation, system_prompt, entry, state->config.context);
  state->turn_scratch = snapshot.messages;
  if (UseBriefStream(state)) {
    if (TryPendingLocalAnswer(state) || TryLocalAppAction(state)) {
      return;
    }
    // Context layout: [system, history..., pending user]; the policy appends the pending user message last.
    std::vector<BriefAiHistoryTurn> history;
    for (size_t i = 1; i + 1 < snapshot.messages.size(); ++i) {
      history.push_back(BriefAiHistoryTurn{.role = snapshot.messages[i].role, .content = snapshot.messages[i].content});
    }
    const auto& summary = state->conversation.Summary();
    StreamBriefTurn(state, std::move(history), summary ? summary->text : std::string());
    return;
  }
  RunNonStreamTurn(state);
}

void AgentSession::RefreshCompactionService(const std::shared_ptr<Impl>& state) {
  if (state->thread_store && state->llm) {
    state->compaction = std::make_unique<ThreadCompactionService>(*state->thread_store, *state->thread_store, *state->thread_store, state->llm.get());
  } else {
    state->compaction.reset();
  }
}

// A submit made while ConfigureOnIO runs must not be lost: checking `configured` and setting
// submit_when_ready under configure_work_mutex (which ConfigureOnIO holds until it has consumed the flag)
// means the flag is either set before ConfigureOnIO looks at it, or the turn sees `configured` and starts.
// True: start the turn now (outside the lock: the turn may take configure_work_mutex itself). After a failed
// configure nothing would start a queued turn, so it ends here with the error instead of staying busy.
bool AgentSession::StartNowOrWhenConfigured(const std::shared_ptr<Impl>& state) {
  std::unique_lock work_lock(state->configure_work_mutex);
  if (state->configured) {
    return true;
  }
  if (!state->configure_failed) {
    state->submit_when_ready = true;
    return false;
  }
  work_lock.unlock();
  PushError(state, "Agent configure failed");
  FinishTurn(state);
  return false;
}

void AgentSession::ConfigureOnIO(const std::shared_ptr<Impl>& state) {
  // Configure() counted this run and releases it when the posted closure is destroyed.
  // Serialize ConfigureOnIO body — concurrent Configure() raced mcp.Start/Stop (UAF → SEGV in logger).
  // Cancel() means "abort the active turn", not "abort agent configure". OnNewChat /
  // find-someone calls Cancel while Me→ReloadFromDisk may still be reconfiguring; tearing
  // down llm here left the session unconfigured and the next send showed MissingKey.
  // Do not PauseBackgroundWork here: this already runs on a worker pool thread. Pausing blocked
  // concurrent posts (profile unlock, inbox) until Resume — and a missed Resume left
  // unlock_in_progress stuck on Android.
  std::unique_lock work_lock(state->configure_work_mutex);
  // A failed configure ends a question queued for it (its error, then loading off) instead of leaving it busy.
  const auto end_queued_submit = [&] {
    const bool queued = std::exchange(state->submit_when_ready, false);
    work_lock.unlock();
    if (queued) {
      FinishTurn(state);
    }
  };
  state->configure_failed = false;
  try {
    LlmConfig llm_config = state->config.llm;
    if (ResolvePreset(state->config) == "brief") {
      llm_config.require_api_key = true;
    }
    state->llm = std::make_unique<LlmClient>(llm_config);
    // Streamed answers go through the gateway's /pp/chat/stream (live since app-static-api #169);
    // PP_BROWSER_BRIEF_STREAM_URL only redirects them to a local fake server for development.
    {
      const auto brief_client = std::make_shared<BriefAiClient>(llm_config, BriefStreamUrlOverride());
      state->brief_stream = [brief_client](const BriefAiRequest& request,
                                           const std::function<void(const BriefAiEvent&)>& on_event,
                                           const std::atomic<bool>& cancel) {
        return brief_client->Stream(request, on_event, cancel);
      };
    }

    const AppConfig defaults = Config::DefaultAppConfig();
    McpClient::SetHttpPost([](const std::string& url, const std::string& body,
                              const std::map<std::string, std::string>& headers, const long timeout_s) {
      HttpTimeout timeout;
      if (timeout_s > 0) {
        timeout.total_s = timeout_s;
      }
      return HttpClient::Post(url, body, headers, kMaxHttpClientBodyBytes, timeout);
    });
    state->mcp.Start(state->config, defaults);

    std::vector<std::string> custom_prefixes;
    custom_prefixes.reserve(state->config.mcp_servers.size());
    for (const McpConfig& entry : state->config.mcp_servers) {
      custom_prefixes.push_back(entry.id);
    }

    BuildToolRegistryFromConfig(state->tools, state->config, state->mcp.PromotedPtr(), state->mcp.CustomPtrs(),
                                custom_prefixes);
    if (state->tool_registration_hook) {
      state->tool_registration_hook(state->tools);
    }
    state->configured = true;
    RefreshCompactionService(state);
    {
      std::lock_guard lock(state->app_action_mutex);
      state->app_action_brief = ResolvePreset(state->config) == "brief";
      state->app_action_tools = AppActionCapabilities(state);
    }

    state->Log().info << "Agent configured with " << state->tools.Tools().size() << " tool(s)";
    for (const ToolDescriptor& tool : state->tools.Tools()) {
      state->Log().info << "  - " << tool.definition.name;
    }
  } catch (const std::exception& e) {
    state->configured = false;
    state->configure_failed = true;
    state->mcp.Stop();
    state->llm.reset();
    state->tools.Clear();
    RefreshCompactionService(state);
    state->Log().error << "Agent configure failed: " << e.what();
    PushError(state, std::string("Agent configure failed: ") + e.what());
    end_queued_submit();
    return;
  } catch (...) {
    state->configured = false;
    state->configure_failed = true;
    state->mcp.Stop();
    state->llm.reset();
    state->tools.Clear();
    RefreshCompactionService(state);
    state->Log().error << "Agent configure failed: unknown exception";
    PushError(state, "Agent configure failed");
    end_queued_submit();
    return;
  }

  const bool submit = state->submit_when_ready && !state->pending_user_text.empty();
  state->submit_when_ready = false;
  // The turn runs outside the lock: a feed turn's re-list takes configure_work_mutex itself.
  work_lock.unlock();
  // Wake the UI loop (PostUI requests a frame) so a send ChatController held while configuring goes out now,
  // not after the power-save wait.
  AppRuntime::PostUI([] {});
  if (!submit) {
    return;
  }
  if (!state->cancelled) {
    StartTurn(state);
  } else {
    // A Cancel raced configure; do not leave busy stuck from Submit, and tell the UI the turn is over (it shows
    // "thinking" in that thread until it hears so).
    state->busy = false;
    PushLoading(state, false, state->pending_thread_id);
  }
}

AgentSession::AgentSession() : impl_(std::make_shared<Impl>()) {
  impl_->conversation.StartNewConversation();
}

AgentSession::~AgentSession() {
  Cancel();
}

void AgentSession::Configure(const AppConfig& config) {
  impl_->config = config;
  impl_->configured = false;
  impl_->cancelled = false;

  // Count the configure as in flight before it is posted, so WaitForConfigureIdle() called right
  // after Configure() cannot return before the worker has even started. The count is released when
  // the posted closure is destroyed — after it ran, or when the runtime drops it during teardown —
  // so a dropped post cannot leave WaitForConfigureIdle() waiting forever.
  struct InflightToken {
    explicit InflightToken(std::shared_ptr<Impl> owner) : impl(std::move(owner)) {}
    InflightToken(const InflightToken&) = delete;
    InflightToken& operator=(const InflightToken&) = delete;
    ~InflightToken() {
      std::lock_guard lock(impl->configure_mutex);
      --impl->configure_inflight;
      impl->configure_cv.notify_all();
    }
    std::shared_ptr<Impl> impl;
  };
  {
    std::lock_guard lock(impl_->configure_mutex);
    ++impl_->configure_inflight;
  }
  AppRuntime::PostWorkerNormal(
      [impl = impl_, token = std::make_shared<InflightToken>(impl_)]() { ConfigureOnIO(impl); });
}

void AgentSession::SetToolRegistrationHook(ToolRegistrationHook hook) {
  impl_->tool_registration_hook = std::move(hook);
}

void AgentSession::SetBriefAiStream(BriefAiStreamFn fn) {
  impl_->brief_stream_override = std::move(fn);
}

void AgentSession::SetMissingToolWait(const std::chrono::milliseconds total, const std::chrono::milliseconds poll) {
  impl_->missing_tool_wait = total;
  impl_->missing_tool_poll = poll;
}

void AgentSession::SetLocalContactsSource(LocalContactsFn source) {
  impl_->local_contacts = std::move(source);
}

void AgentSession::SetLocalAskTtl(const std::chrono::milliseconds ttl) {
  impl_->local_ask_ttl = ttl;
}

bool AgentSession::IsAppActionRequest(const std::string& text) const {
  std::lock_guard lock(impl_->app_action_mutex);
  return impl_->app_action_brief && ClassifyAppActionMatch(text, impl_->app_action_tools).has_value();
}

void AgentSession::SubmitLocalAction(const std::string& thread_id, const std::string& user_text,
                                     const std::string& tool, Object args, const bool in_peer_chat) {
  if (!IsDirectActionTool(tool)) {
    impl_->Log().warning << "local action payload with a tool outside the whitelist ignored: " << tool;
    return;
  }
  if (impl_->busy.exchange(true)) {
    impl_->Log().warning << "local action dropped: a turn is still running (" << tool << ")";
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = user_text.empty() ? tool : user_text;
  impl_->pending_user_payload.reset();
  impl_->pending_image.reset();
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = in_peer_chat ? AgentTurnMode::ScopedAssist
                                  : (thread_id.empty() ? AgentTurnMode::Conversation : AgentTurnMode::Thread);
  impl_->assist_mode = AtAiMode::Local;
  impl_->pending_direct_action = std::make_pair(tool, std::move(args));

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!StartNowOrWhenConfigured(impl)) {
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SetToolPermissions(ToolPermissionsPrefs permissions) {
  impl_->tool_permissions = std::move(permissions);
}

void AgentSession::SetToolPermissionsSaver(ToolPermissionsSaveFn saver) {
  impl_->tool_permissions_saver = std::move(saver);
}

McpClient* AgentSession::PromotedMcp() {
  return impl_->mcp.PromotedPtr();
}

Roe<void> AgentSession::ResumeToolPermission(const std::string& approval_id, const std::string& decision,
                                             const std::string& decision_label) {
  if (approval_id.empty()) {
    return Error(ApprovalResumeErrorCode(ApprovalResumeError::NotFound));
  }
  if (decision != "allow_once" && decision != "allow_always" && decision != "deny") {
    return Error(ApprovalResumeErrorCode(ApprovalResumeError::InvalidDecision));
  }

  {
    std::lock_guard lock(impl_->park_mutex);
    const ApprovalResumeError err =
        ValidateParkedResume(impl_->parked_approval, approval_id, impl_->busy.load());
    if (err != ApprovalResumeError::Ok) {
      return Error(ApprovalResumeErrorCode(err));
    }
  }

  AppRuntime::PostWorkerNormal(
      [impl = impl_, approval_id, decision, decision_label]() {
        ResumeToolPermissionOnWorker(impl, approval_id, decision, decision_label);
      });
  return {};
}

bool AgentSession::IsConfigured() const {
  return impl_->configured;
}

const Conversation& AgentSession::conversation() const {
  return impl_->conversation;
}

TranscriptEntry& AgentSession::AppendUserMessage(const std::string& user_text,
                                                  std::optional<std::string> user_payload) {
  return impl_->conversation.AppendUser(user_text, std::move(user_payload));
}

bool AgentSession::CompleteAssistantMessage(const std::string& entry_id, const std::string& assistant_raw) {
  return impl_->conversation.CompleteTurn(entry_id, assistant_raw);
}

bool AgentSession::SetAssistantDisplay(const std::string& entry_id, const std::string& assistant_rml,
                                       std::vector<TranscriptChatAction> chat_actions) {
  return impl_->conversation.SetAssistantDisplay(entry_id, assistant_rml, std::move(chat_actions));
}

void AgentSession::SetThreadStore(IThreadStore* store) {
  impl_->thread_store = store;
  RefreshCompactionService(impl_);
}

void AgentSession::BeginStreamTurn() {
  std::lock_guard lock(impl_->stream_cancel_mutex);
  impl_->stream_cancel = std::make_shared<std::atomic<bool>>(false);
  impl_->turn_generation.fetch_add(1, std::memory_order_acq_rel);
}

void AgentSession::Submit(const std::string& user_text, std::optional<std::string> user_payload) {
  if (user_text.empty()) {
    return;
  }
  if (impl_->busy.exchange(true)) {
    // The UI queues AI sends while a turn runs (B112); reaching this means a send slipped past it.
    impl_->Log().warning << "send dropped: a turn is still running";
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image.reset();
  impl_->pending_direct_action.reset();
  impl_->turn_mode = AgentTurnMode::Conversation;
  impl_->pending_thread_id.clear();

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!StartNowOrWhenConfigured(impl)) {
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitToThread(const std::string& thread_id, const std::string& user_text,
                                  std::optional<std::string> user_payload, std::optional<AgentImageTurn> image) {
  if (user_text.empty()) {
    return;
  }
  if (impl_->busy.exchange(true)) {
    // The UI queues AI sends while a turn runs (B112); reaching this means a send slipped past it.
    impl_->Log().warning << "send dropped: a turn is still running";
    return;
  }

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = user_text;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image = std::move(image);
  impl_->pending_direct_action.reset();
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::Thread;

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!StartNowOrWhenConfigured(impl)) {
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::SubmitScopedAssist(const std::string& thread_id, const std::string& prompt,
                                      std::optional<std::string> user_payload, const AtAiMode mode) {
  if (prompt.empty()) {
    return;
  }
  if (impl_->busy.exchange(true)) {
    // The chat already shows "thinking" for this send; say why nothing follows.
    impl_->Log().warning << "scoped assist dropped: a turn is still running (mode=" << static_cast<int>(mode) << ")";
    return;
  }
  impl_->Log().info << "scoped assist mode=" << static_cast<int>(mode) << " prompt_chars=" << prompt.size()
                    << " configured=" << (impl_->configured ? 1 : 0);

  impl_->cancelled = false;
  BeginStreamTurn();
  impl_->pending_user_text = prompt;
  impl_->pending_user_payload = std::move(user_payload);
  impl_->pending_image.reset();
  impl_->pending_direct_action.reset();
  impl_->pending_thread_id = thread_id;
  impl_->turn_mode = AgentTurnMode::ScopedAssist;
  impl_->assist_mode = mode;

  AppRuntime::PostWorkerNormal([impl = impl_]() {
    if (!StartNowOrWhenConfigured(impl)) {
      return;
    }
    StartTurn(impl);
  });
}

void AgentSession::PollEvents(std::vector<AgentEvent>& out) {
  std::lock_guard lock(impl_->event_mutex);
  out.insert(out.end(), impl_->pending_events.begin(), impl_->pending_events.end());
  impl_->pending_events.clear();
}

void AgentSession::Cancel() {
  impl_->cancelled = true;
  {
    std::lock_guard lock(impl_->stream_cancel_mutex);
    impl_->stream_cancel->store(true);
  }
  impl_->busy = false;
  impl_->tool_registration_hook = nullptr;
  // A permission card waiting in some thread is left alone: stopping a turn (or pausing the app) is not an answer to
  // it, and a parked request belongs to a turn that already ended. It goes stale with a new turn in its own thread.
}

void AgentSession::WaitForConfigureIdle() {
  std::unique_lock lock(impl_->configure_mutex);
  impl_->configure_cv.wait(lock, [&] { return impl_->configure_inflight == 0; });
}

void AgentSession::ForgetThread(const std::string& thread_id) {
  std::lock_guard lock(impl_->last_role_mutex);
  impl_->last_role_by_thread.erase(thread_id);
}

void AgentSession::StartNewConversation() {
  AppRuntime::PostWorkerNormal([impl = impl_]() {
    CancelParkedApproval(impl, ParkedApprovalState::Cancelled);
    impl->conversation.StartNewConversation();
    impl->pending_local_action.reset(); // a question asked in the old conversation is not answered in the new one
    impl->turn_scratch.clear();
    impl->pending_entry_id.clear();
    impl->pending_user_text.clear();
    impl->pending_user_payload.reset();
    impl->pending_image.reset();
    impl->cancelled = false;
    impl->busy = false;
  });
}

} // namespace pbr
