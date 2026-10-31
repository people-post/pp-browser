#pragma once

// Shared by the AgentSession tests: a fake brief_AI stream, an in-memory thread store, a counter of completions
// requests, and an event pump.

#include "feature/ai/AgentSession.h"

#include "lib/ai/conversation/Conversation.h"
#include "foundation/data/Config.h"
#include "foundation/error/AppError.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/runtime/ProductOwners.h"
#include "common/thread/IThreadStore.h"
#include "lib/ai/LlmClient.h"
#include "lib/ai/ToolRegistry.h"
#include "lib/ai/mcp/McpClient.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::agent_test {


inline BriefAiEvent Meta() {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Meta;
  event.route = "chat";
  return event;
}

inline BriefAiEvent Status(const std::string& tool, const std::string& phase) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Status;
  event.tool = tool;
  event.phase = phase;
  return event;
}

inline BriefAiEvent Token(const std::string& delta) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Token;
  event.delta = delta;
  return event;
}

inline BriefAiEvent Done(const std::string& response, const std::string& role = {}) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Done;
  event.response = response;
  event.role = role;
  event.finish = "stop";
  event.sources.push_back(BriefAiSource{.title = "T", .url = "https://example.com/a", .kind = "web"});
  return event;
}

inline BriefAiEvent ErrorEvent(const std::string& message, const bool retryable) {
  BriefAiEvent event;
  event.type = BriefAiEvent::Type::Error;
  event.code = "provider_unavailable";
  event.message = message;
  event.retryable = retryable;
  return event;
}

// Network-free config: no MCP (the promoted entry is present but disabled so the default Brief MCP URL is not used),
// the local pipeline's LLM points at a closed port so a handoff fails fast.
inline AppConfig MakeConfig(const std::string& preset) {
  AppConfig config;
  config.llm.preset = preset;
  config.llm.api_key = "dummy-key";
  config.llm.base_url = "http://127.0.0.1:1";
  config.llm.model = preset == "brief" ? "xai" : "llama3.2";
  config.llm.require_api_key = true;
  config.promoted_mcp.id = "off";
  config.promoted_mcp.url = "http://127.0.0.1:1/mcp";
  config.promoted_mcp.enabled = false;
  config.mcp_servers.clear();
  return config;
}

// Keeps messages in memory; everything the AI thread path does not touch is a stub.
class MemoryThreadStore : public IThreadStore {
public:
  std::vector<ThreadMessage> messages;

  void Flush() override {}
  Roe<std::vector<Thread>> ListThreads() const override { return std::vector<Thread>{}; }
  Roe<std::optional<Thread>> GetThread(const std::string&) const override { return std::optional<Thread>{}; }
  Roe<Thread> UpsertThread(const Thread& thread) override { return thread; }
  Roe<bool> DeleteThread(const std::string&) override { return true; }
  Roe<std::optional<Thread>> FindDirectThread(const DirectChatTarget&) const override { return std::optional<Thread>{}; }
  Roe<Thread> FindOrCreateDirectThread(const DirectChatTarget&, const std::string&, const std::string&) override { return Thread{}; }
  Roe<std::optional<Thread>> FindGroupThread(const std::string&) const override { return std::optional<Thread>{}; }
  Roe<Thread> FindOrCreateGroupThread(const std::string&, const std::string&, const std::vector<std::string>&) override {
    return Thread{};
  }
  Roe<std::vector<ThreadMessage>> GetMessages(const std::string&) const override { return messages; }
  Roe<std::vector<ThreadMessage>> GetMessagesPage(const std::string&, std::optional<int64_t>, size_t) const override {
    return messages;
  }
  Roe<std::vector<ThreadMessage>> GetMessagesForContext(const std::string&, const ContextBudget&) const override {
    return messages;
  }
  Roe<int64_t> CountContextEligibleMessagesAfter(const std::string&, int64_t) const override { return int64_t{0}; }
  Roe<int64_t> CountAnnotationsForTarget(const std::string&, const std::string&) const override { return int64_t{0}; }
  Roe<std::vector<ThreadMessage>> GetContextEligibleMessagesAfter(const std::string&, int64_t) const override {
    return std::vector<ThreadMessage>{};
  }
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override {
    ThreadMessage stored = message;
    stored.display_order = static_cast<int64_t>(messages.size()) + 1;
    messages.push_back(stored);
    return stored;
  }
  Roe<bool> UpdateMessage(const ThreadMessage& message) override {
    for (ThreadMessage& stored : messages) {
      if (stored.id == message.id) {
        stored = message;
        return true;
      }
    }
    return false;
  }
  Roe<bool> HasMessageId(const std::string&, const std::string&) const override { return false; }
  Roe<void> ClearMessages(const std::string&, const ClearMessagesOptions&) override { return {}; }
  Roe<std::vector<ThreadMessage>> ExportMessagesUpTo(const std::string&, const std::optional<std::string>&) const override {
    return messages;
  }
  Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string&) const override {
    return std::optional<ConversationSummary>{};
  }
  Roe<void> SetThreadMemory(const std::string&, const ConversationSummary&) override { return {}; }
  Roe<void> ClearThreadMemory(const std::string&) override { return {}; }
  Roe<uint64_t> AllocateSenderSeq(const std::string&) override { return uint64_t{0}; }
  Roe<uint32_t> GetChatTargetSessionEpoch(const std::string&) const override { return uint32_t{0}; }
  Roe<std::vector<ThreadMessage>> GetMessagesBySeqRange(const std::string&, const SeqRangeQuery&) const override {
    return std::vector<ThreadMessage>{};
  }
  Roe<PeerSyncState> GetPeerSyncState(const std::string&, uint32_t, const std::string&) const override {
    return PeerSyncState{};
  }
  Roe<void> SetPeerSyncState(const std::string&, uint32_t, const PeerSyncState&, const std::string&) override {
    return {};
  }
  Roe<void> CancelOldEpochPending(const std::string&, uint32_t) override { return {}; }
  Roe<void> AdoptChatTargetEpoch(const std::string&, uint32_t) override { return {}; }
  Roe<ThreadMessage> AppendMessageWithPassiveEpochAdopt(const ThreadMessage& message, uint32_t, uint32_t,
                                                        const PeerSyncState&) override {
    return message;
  }
  Roe<uint32_t> BumpLocalChatTargetEpoch(const std::string&) override { return uint32_t{0}; }
  Roe<void> ReconcileOutbox() override { return {}; }
  Roe<std::vector<std::pair<std::string, std::string>>> ListPendingOutbox() const override {
    return std::vector<std::pair<std::string, std::string>>{};
  }
};

inline AgentImageTurn TestImage(const std::string& message_id = {}) {
  return AgentImageTurn{.image = BriefAiImage{.mime = "image/jpeg", .data = {0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3}},
                        .message_id = message_id,
                        .ai_image_json = message_id.empty() ? std::string() : R"({"content_hash":"ab"})"};
}

class AgentSessionBriefStreamTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(ProductRuntimeConfig());
    // Every completions request (the model pipeline, output repair, compaction) is counted and kept here.
    LlmClient::SetRequestObserverForTests([this](const std::string& body) {
      std::lock_guard lock(mu_);
      llm_bodies_.push_back(body);
    });
  }
  void TearDown() override {
    session_.reset();
    AppRuntime::Shutdown();
    LlmClient::SetRequestObserverForTests(nullptr);
  }

  void Start(const std::string& preset = "brief") { StartWith(MakeConfig(preset)); }

  void StartWith(const AppConfig& config) {
    session_ = std::make_unique<AgentSession>();
    session_->Configure(config);
    session_->WaitForConfigureIdle(); // Configure() counts the run before posting it, so this is enough
    ASSERT_TRUE(session_->IsConfigured());
  }

  // Installs a fake stream replaying `events` and returning `outcome`.
  void Script(std::vector<BriefAiEvent> events, Roe<BriefAiOutcome> outcome) {
    session_->SetBriefAiStream([this, events = std::move(events), outcome = std::move(outcome)](
                                   const BriefAiRequest& request,
                                   const std::function<void(const BriefAiEvent&)>& on_event,
                                   const std::atomic<bool>&) -> Roe<BriefAiOutcome> {
      Record(request);
      for (const BriefAiEvent& event : events) {
        on_event(event);
      }
      return outcome;
    });
  }

  void Record(const BriefAiRequest& request) {
    std::lock_guard lock(mu_);
    requests_.push_back(request);
  }

  std::vector<BriefAiRequest> Requests() {
    std::lock_guard lock(mu_);
    return requests_;
  }

  /** Bodies of the /chat/completions requests made so far (the brief preset must make none). */
  std::vector<std::string> LlmRequests() {
    std::lock_guard lock(mu_);
    return llm_bodies_;
  }

  // Polls until a LoadingChanged(false) arrives (or `stop_when` returns true); fails on timeout.
  std::vector<AgentEvent> WaitForTurn(const std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    std::vector<AgentEvent> all;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      std::vector<AgentEvent> batch;
      AppRuntime::RunUITasks(); // events are posted to the UI mailbox
      session_->PollEvents(batch);
      for (AgentEvent& event : batch) {
        all.push_back(std::move(event));
      }
      for (const AgentEvent& event : all) {
        if (event.type == AgentEventType::LoadingChanged && !event.loading) {
          return all;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "turn did not finish within the bound";
    return all;
  }

  static std::vector<AgentEvent> Of(const std::vector<AgentEvent>& events, const AgentEventType type) {
    std::vector<AgentEvent> out;
    for (const AgentEvent& event : events) {
      if (event.type == type) {
        out.push_back(event);
      }
    }
    return out;
  }

  std::unique_ptr<AgentSession> session_;
  std::mutex mu_;
  std::vector<BriefAiRequest> requests_;
  std::vector<std::string> llm_bodies_;
};

} // namespace pbr::agent_test
