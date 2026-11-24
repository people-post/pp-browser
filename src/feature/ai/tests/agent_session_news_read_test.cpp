// A news item read in an AI thread (DEV/PLAN-news-inline-digest.md, brief_AI contract section 15): the request and
// the digest are stored hidden, the text streams to the reader as deltas (the chat screen keeps them out of the chat)
// and only done's text is stored, failures store no digest, every brief turn declares `news_digest`, and the latest
// news list stays in the history sent.
#include "common/Metrics.h"
#include "feature/ai/tests/agent_session_test_support.h"

#include <algorithm>
#include <memory>
#include <mutex>

namespace {

using namespace pbr;
using namespace pbr::agent_test;

const char* kRequest = "https://n.example/a\n这篇讲了什么";

BriefAiEvent DoneKind(const std::string& response, const std::string& kind) {
  BriefAiEvent event = Done(response);
  event.sources.clear();
  event.answer_kind = kind;
  return event;
}

ThreadMessage Stored(const std::string& id, const std::string& sender, const std::string& text, const int64_t order,
                     const char* generation = nullptr) {
  ThreadMessage message;
  message.id = id;
  message.thread_id = "t1";
  message.sender_contact_id = sender;
  message.text = text;
  message.display_order = order;
  if (generation) {
    message.generation = generation;
  }
  return message;
}

const char* kList =
    "1. [A](https://n.example/a \"pp:news\")\n2. [B](https://n.example/b \"pp:news\")\n"
    "3. [C](https://n.example/c \"pp:news\")\n4. [D](https://n.example/d \"pp:news\")";

/** The "Metrics" channel's lines (a logger's handlers cannot be removed: one capture for the process). */
struct MetricsCapture : logging::Handler {
  static MetricsCapture& Instance() {
    static const std::shared_ptr<MetricsCapture> capture = [] {
      auto made = std::make_shared<MetricsCapture>();
      MetricsLog().addHandler(made);
      return made;
    }();
    return *capture;
  }
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mu);
    lines.push_back(message);
  }
  std::vector<std::string> Take(const std::string& event) {
    std::lock_guard lock(mu);
    std::vector<std::string> found;
    for (const std::string& line : lines) {
      if (line.find("event=" + event + " ") != std::string::npos) {
        found.push_back(line.substr(line.find("event=")));
      }
    }
    lines.clear();
    return found;
  }
  std::mutex mu;
  std::vector<std::string> lines;
};

bool HasNewsDigest(const BriefAiRequest& request) {
  return std::find(request.capabilities.begin(), request.capabilities.end(), "news_digest") !=
         request.capabilities.end();
}

TEST_F(AgentSessionBriefStreamTest, ANewsReadStoresAHiddenRequestAndDigest) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("来源：n.example\n\n文章说……"), DoneKind("来源：n.example\n\n文章说……", "news_digest")},
         BriefAiOutcome::Done);
  ASSERT_EQ(session_->SubmitNewsRead("t1", kRequest, "Rates rise"), SubmitOutcome::Started);
  const auto events = WaitForTurn();

  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_EQ(requests[0].message, kRequest) << "two lines, no title";
  EXPECT_EQ(requests[0].news_title, "Rates rise") << "the title is data beside the message";
  EXPECT_FALSE(requests[0].intent.has_value()) << "no new intent: an unknown one is a 400";
  EXPECT_TRUE(HasNewsDigest(requests[0]));

  // An older server's one big token: one delta for the reader (AgentTurnController keeps it out of the chat).
  const auto deltas = Of(events, AgentEventType::AssistantDelta);
  ASSERT_EQ(deltas.size(), 1u);
  EXPECT_EQ(deltas[0].thread_id, "t1");
  EXPECT_EQ(deltas[0].text, "来源：n.example\n\n文章说……");
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].answer_kind, "news_digest");

  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[0].sender_contact_id, kLocalSelfContactId);
  EXPECT_EQ(store.messages[0].text, kRequest);
  EXPECT_EQ(store.messages[0].generation.value_or(""), kNewsRequestGeneration);
  EXPECT_EQ(store.messages[1].sender_contact_id, kAiAssistantContactId);
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsDigestGeneration);
  EXPECT_FALSE(IsLocalActionMessage(store.messages[0])) << "it stays in the history sent to the model";
}

// done.translated on a digest: stored as the translated digest (hidden and read like any digest).
TEST_F(AgentSessionBriefStreamTest, ATranslatedDigestIsStoredAsSuch) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  BriefAiEvent done = DoneKind("来源：Reuters\n\n文章说……", "news_digest");
  done.translated = true;
  Script({Meta(), done}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  WaitForTurn();
  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsDigestTranslatedGeneration);
  EXPECT_TRUE(IsHiddenNewsMessage(store.messages[1]));
  EXPECT_TRUE(IsNewsDigestMessage(store.messages[1]));
}

// A streamed digest (the source line first, then the text): the reader gets the text as it grows, and what is stored
// is done's response, not the streamed text.
TEST_F(AgentSessionBriefStreamTest, AStreamedDigestReachesTheReaderAndOnlyDonesTextIsStored) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  session_->SetBriefAiStream([this](const BriefAiRequest& request,
                                    const std::function<void(const BriefAiEvent&)>& on_event,
                                    const std::atomic<bool>&) -> Roe<BriefAiOutcome> {
    Record(request);
    on_event(Meta());
    on_event(Token("n.example \u00b7 2026-11-24\n\n"));
    std::this_thread::sleep_for(std::chrono::milliseconds(80)); // past the session's 50 ms delta cap
    on_event(Token("The article says rates rose."));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    on_event(Token(" Markets fell."));
    BriefAiEvent done = Done("n.example \u00b7 2026-11-24\n\nThe article says rates rose. Markets fell.");
    done.sources.clear();
    done.answer_kind = "news_digest";
    on_event(done);
    return BriefAiOutcome::Done;
  });
  session_->SubmitNewsRead("t1", kRequest, "Rates rise");
  const auto events = WaitForTurn();
  const auto deltas = Of(events, AgentEventType::AssistantDelta);
  ASSERT_GE(deltas.size(), 2u);
  EXPECT_EQ(deltas[0].text, "n.example \u00b7 2026-11-24\n\n") << "the source line comes first";
  for (size_t i = 1; i < deltas.size(); ++i) {
    EXPECT_EQ(deltas[i].text.rfind(deltas[i - 1].text, 0), 0u) << "each delta is the whole text so far";
  }
  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[1].text, "n.example \u00b7 2026-11-24\n\nThe article says rates rose. Markets fell.");
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsDigestGeneration);
}

// Tokens were streamed, then done says the page was not the article: done's fixed sentence is the answer, stored as
// "could not be read" (the item stays unread), and the streamed text is never stored.
TEST_F(AgentSessionBriefStreamTest, UnavailableAfterTokensStoresOnlyTheFailure) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  BriefAiEvent done = DoneKind("这篇暂时取不到内容。", "news_unavailable");
  done.unavailable_kind = "mismatch";
  Script({Meta(), Token("n.example \u00b7 11月24日\n\n"), Token("文章说……"), done}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  const auto events = WaitForTurn();
  EXPECT_FALSE(Of(events, AgentEventType::AssistantDelta).empty()) << "the reader saw the text while it came";
  const auto ready = Of(events, AgentEventType::AssistantReady);
  ASSERT_EQ(ready.size(), 1u);
  EXPECT_EQ(ready[0].answer_kind, "news_unavailable");
  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[1].text, "这篇暂时取不到内容。");
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsFailedGeneration);
}

// "ai.news_read" counts reads by outcome, the server's reason for no digest, and whether "Try again" started the read:
// fixed words only, never the URL, the title or any text.
TEST_F(AgentSessionBriefStreamTest, TheNewsReadMetricSaysTheOutcomeTheReasonAndARetry) {
  MetricsCapture& metrics = MetricsCapture::Instance();
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  metrics.Take("ai.news_read");

  BriefAiEvent unavailable = DoneKind("这篇暂时取不到内容。", "news_unavailable");
  unavailable.unavailable_kind = "blocked";
  Script({Meta(), unavailable}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest, "Rates rise");
  WaitForTurn();
  EXPECT_EQ(metrics.Take("ai.news_read"),
            std::vector<std::string>{"event=ai.news_read answer=news_unavailable unavailable_kind=blocked "
                                     "translated=0 retry=0"});

  BriefAiEvent digest = DoneKind("n.example\n\nThe article says…", "news_digest");
  digest.translated = true;
  Script({Meta(), Token("n.example\n\n"), digest}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest, "Rates rise", /*retry=*/true);
  WaitForTurn();
  const std::vector<std::string> lines = metrics.Take("ai.news_read");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "event=ai.news_read answer=news_digest unavailable_kind=- translated=1 retry=1");
  EXPECT_EQ(lines[0].find("n.example"), std::string::npos);
  EXPECT_EQ(lines[0].find("Rates"), std::string::npos);

  // A new server's unknown reason reads "other".
  BriefAiEvent odd = DoneKind("x", "news_unavailable");
  odd.unavailable_kind = "https://n.example/a";
  Script({Meta(), odd}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  WaitForTurn();
  EXPECT_EQ(metrics.Take("ai.news_read"),
            std::vector<std::string>{"event=ai.news_read answer=news_unavailable unavailable_kind=other "
                                     "translated=0 retry=0"});
}

TEST_F(AgentSessionBriefStreamTest, AnUnreadableArticleIsStoredAsFailed) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("这篇暂时取不到内容。"), DoneKind("这篇暂时取不到内容。", "news_unavailable")},
         BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  WaitForTurn();
  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsFailedGeneration);
}

// Only an explicit "news_digest" is a digest: no answer_kind (an older server), "news_pick" or anything else is stored
// as "could not be read" (hidden, not read).
TEST_F(AgentSessionBriefStreamTest, OnlyAnExplicitDigestCountsAsRead) {
  for (const std::string kind : {"", "news_pick", "something_new"}) {
    Start();
    MemoryThreadStore store;
    session_->SetThreadStore(&store);
    Script({Meta(), DoneKind("some answer", kind)}, BriefAiOutcome::Done);
    session_->SubmitNewsRead("t1", kRequest);
    WaitForTurn();
    ASSERT_EQ(store.messages.size(), 2u) << kind;
    EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsFailedGeneration) << kind;
    session_.reset();
  }
}

// A digest cut at the length limit is still a digest (brief_AI adds its own "only the first part" line).
TEST_F(AgentSessionBriefStreamTest, ALengthCutDigestIsStillADigest) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  BriefAiEvent done = DoneKind("long digest", "news_digest");
  done.finish = "length";
  Script({Meta(), done}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  WaitForTurn();
  ASSERT_EQ(store.messages.size(), 2u);
  EXPECT_EQ(store.messages[1].generation.value_or(""), kNewsDigestGeneration);
}

// brief_AI handing a news read back as an app action: nothing local runs, nothing is shown or stored.
TEST_F(AgentSessionBriefStreamTest, AHandedBackNewsReadEndsUnreadWithNoLocalAnswer) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  BriefAiEvent handoff;
  handoff.type = BriefAiEvent::Type::Handoff;
  handoff.capability = "add_contact";
  handoff.reason = "app_action";
  Script({Meta(), handoff}, BriefAiOutcome::Handoff);
  session_->SubmitNewsRead("t1", kRequest);
  const auto events = WaitForTurn();
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_TRUE(LlmRequests().empty());
  ASSERT_EQ(store.messages.size(), 1u);
  EXPECT_EQ(store.messages[0].generation.value_or(""), kNewsRequestGeneration);
}

// An error after part of the text: that part is no digest, so nothing is stored and the item stays unread.
TEST_F(AgentSessionBriefStreamTest, ANewsReadErrorStoresNoAnswer) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Token("par"), ErrorEvent("模型暂时不可用", true)}, BriefAiOutcome::Error);
  session_->SubmitNewsRead("t1", kRequest);
  const auto events = WaitForTurn();
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_EQ(Of(events, AgentEventType::Error).size(), 1u);
  ASSERT_EQ(store.messages.size(), 1u);
  EXPECT_EQ(store.messages[0].generation.value_or(""), kNewsRequestGeneration);
}

TEST_F(AgentSessionBriefStreamTest, AStoppedNewsReadStoresNoAnswer) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  std::atomic<bool> sent_token{false};
  session_->SetBriefAiStream([this, &sent_token](const BriefAiRequest& request,
                                                 const std::function<void(const BriefAiEvent&)>& on_event,
                                                 const std::atomic<bool>& cancel) -> Roe<BriefAiOutcome> {
    Record(request);
    on_event(Meta());
    on_event(Token("part"));
    sent_token = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!cancel.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return BriefAiOutcome::Cancelled;
  });
  session_->SubmitNewsRead("t1", kRequest);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!sent_token.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(sent_token.load());
  session_->Cancel();
  const auto events = WaitForTurn();
  EXPECT_TRUE(Of(events, AgentEventType::AssistantReady).empty());
  EXPECT_EQ(Of(events, AgentEventType::AssistantDelta).size(), 1u) << "the reader saw the part; nothing keeps it";
  ASSERT_EQ(store.messages.size(), 1u);
  EXPECT_EQ(store.messages[0].generation.value_or(""), kNewsRequestGeneration);
}

// The next ordinary question in the thread is an ordinary turn again.
TEST_F(AgentSessionBriefStreamTest, AfterANewsReadTheNextQuestionIsStoredAsUsual) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), DoneKind("digest", "news_digest")}, BriefAiOutcome::Done);
  session_->SubmitNewsRead("t1", kRequest);
  WaitForTurn();
  Script({Meta(), Token("It says 3%"), Done("It says 3%")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "刚才那条里的数字是什么意思");
  const auto events = WaitForTurn();
  EXPECT_FALSE(Of(events, AgentEventType::AssistantDelta).empty());
  ASSERT_EQ(store.messages.size(), 4u);
  EXPECT_FALSE(store.messages[2].generation.has_value());
  EXPECT_FALSE(store.messages[3].generation.has_value());
  // The follow-up carries what was read.
  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  const auto& history = requests[1].history;
  ASSERT_GE(history.size(), 2u);
  EXPECT_EQ(history[history.size() - 2].content, kRequest);
  EXPECT_EQ(history.back().content, "digest");
}

// Every brief turn of an AI conversation declares `news_digest` (so lists come back marked); "@ai" in a chat and an
// image turn declare nothing.
TEST_F(AgentSessionBriefStreamTest, ThreadTurnsDeclareNewsDigest) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  Script({Meta(), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "今天有什么新闻");
  WaitForTurn();
  session_->SubmitScopedAssist("t2", "Is this true?", std::nullopt, AtAiMode::Local);
  WaitForTurn();
  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 2u);
  EXPECT_TRUE(HasNewsDigest(requests[0]));
  EXPECT_FALSE(HasNewsDigest(requests[1]));
  EXPECT_TRUE(requests[0].news_title.empty()) << "only a news read carries a title";
}

// A few reads pushed the list out of the last six turns: it goes first, the newest five follow.
TEST_F(AgentSessionBriefStreamTest, TheLatestNewsListStaysInTheHistorySent) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  int64_t order = 0;
  store.messages.push_back(Stored("q0", kLocalSelfContactId, "今天有什么新闻", ++order));
  store.messages.push_back(Stored("list", kAiAssistantContactId, kList, ++order));
  for (int i = 0; i < 4; ++i) {
    const std::string n = std::to_string(i);
    store.messages.push_back(Stored("r" + n, kLocalSelfContactId, "https://n.example/" + n + "\n这篇讲了什么", ++order,
                                    kNewsRequestGeneration));
    store.messages.push_back(Stored("d" + n, kAiAssistantContactId, "digest " + n, ++order, kNewsDigestGeneration));
  }
  Script({Meta(), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "把第四条总结给我");
  WaitForTurn();
  const auto requests = Requests();
  ASSERT_EQ(requests.size(), 1u);
  const auto& history = requests[0].history;
  ASSERT_EQ(history.size(), 6u);
  EXPECT_EQ(history[0].role, "assistant");
  EXPECT_EQ(history[0].content, kList);
  EXPECT_EQ(history[1].content, "digest 1");
  EXPECT_EQ(history.back().content, "digest 3");
}

// A single news pick after the list has one news item: it is no list, so the list is the one kept.
TEST_F(AgentSessionBriefStreamTest, AListFollowedByASinglePickKeepsTheList) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  int64_t order = 0;
  store.messages.push_back(Stored("list", kAiAssistantContactId, kList, ++order));
  for (int i = 0; i < 3; ++i) {
    store.messages.push_back(Stored("q" + std::to_string(i), kLocalSelfContactId, "more", ++order));
    store.messages.push_back(Stored("a" + std::to_string(i), kAiAssistantContactId, "answer", ++order));
  }
  store.messages.push_back(Stored("q9", kLocalSelfContactId, "给我一条科技新闻", ++order));
  store.messages.push_back(
      Stored("pick", kAiAssistantContactId, "A pick.\n\n[View details](https://n.example/p \"pp:news\")", ++order));
  Script({Meta(), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "把第四条总结给我");
  WaitForTurn();
  const auto history = Requests().at(0).history;
  ASSERT_EQ(history.size(), 6u);
  EXPECT_EQ(history[0].content, kList);
  EXPECT_EQ(history.back().content, "A pick.\n\n[View details](https://n.example/p \"pp:news\")");
}

// The list still among the last six: nothing is added or moved.
TEST_F(AgentSessionBriefStreamTest, AListInTheLastSixIsSentOnce) {
  Start();
  MemoryThreadStore store;
  session_->SetThreadStore(&store);
  store.messages.push_back(Stored("q0", kLocalSelfContactId, "今天有什么新闻", 1));
  store.messages.push_back(Stored("list", kAiAssistantContactId, kList, 2));
  Script({Meta(), Done("x")}, BriefAiOutcome::Done);
  session_->SubmitToThread("t1", "把第四条总结给我");
  WaitForTurn();
  const auto history = Requests().at(0).history;
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].content, "今天有什么新闻");
  EXPECT_EQ(history[1].content, kList);
}

} // namespace
