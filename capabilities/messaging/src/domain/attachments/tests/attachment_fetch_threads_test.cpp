// B248: the peer transport answers a chat-blob fetch on its own thread (the mesh pump, holding the mesh's io lock).
// That answer must do nothing there but hand the rest to a worker: no server download, no decrypt, no file work and
// no UI post on that thread. Before, the pump ran the server download inline and its progress posted to the UI
// while the main thread waited for the io lock: a deadlock the iOS watchdog killed.
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "domain/attachments/AttachmentFetch.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/sockets/testing/ScriptedHttpServer.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentSuppressionStore.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <gtest/gtest.h>
#include <map>
#include <mutex>
#include <thread>

namespace pbr {
namespace {

using State = AttachmentFetchWorkflow::DownloadState;
using pbr::test::ScriptedHttpServer;
using Bytes = std::vector<uint8_t>;
using PeerAnswer = std::function<void(Roe<Bytes>)>;

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

/** A peer that is always reachable and answers a fetch only when the test hands the answer over. */
class HeldPeer : public IChatBlobPeerClient {
public:
  bool IsPeerReachable(const std::string&) const override { return true; }
  Roe<Bytes> FetchChatBlob(const ChatBlobRequest&) override { return Error("not used"); }
  Roe<void> PushChatBlob(const ChatBlobRequest&, const Bytes&) override { return Error("not used"); }
  void FetchChatBlobAsync(const ChatBlobRequest&, PeerAnswer on_done) override {
    std::lock_guard lock(mutex_);
    answers_.push_back(std::move(on_done));
  }
  /** The oldest fetch's answer, once one was asked for; empty when none came within the wait. */
  PeerAnswer TakeAnswer() {
    PeerAnswer answer;
    (void)WaitFor([&] {
      std::lock_guard lock(mutex_);
      if (answers_.empty()) {
        return false;
      }
      answer = std::move(answers_.front());
      answers_.erase(answers_.begin());
      return true;
    });
    return answer;
  }

private:
  std::mutex mutex_;
  std::vector<PeerAnswer> answers_;
};

/** What a peer answer did on the thread that gave it. */
struct OnAnswerThread {
  bool returned_in_time = false;
  bool ui_task_posted = false;
  int changed_before = 0;
  int changed_after = 0;
};

class FetchThreadsTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    (void)AppRuntime::HasPendingUITasks(); // the UI mailbox exists before a test thread looks at it
    dir_ = std::filesystem::temp_directory_path() / ("pp_fetch_threads_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);

    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test");
    ASSERT_TRUE(identity_->SetDek(ByteVector(kDataEncryptionKeySize, 0x30)));
    ASSERT_TRUE(identity_->LoadOrCreate());
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    Contact peer;
    peer.id = "contact-peer";
    peer.remote.ids.push_back(mesh::ContactId{.kind = mesh::ContactIdKind::Account, .value = "account:peer"});
    SyncContactMirrors(peer);
    ASSERT_TRUE(contacts_->Upsert(peer));
    Thread direct;
    direct.id = "t1";
    direct.kind = ThreadKind::Direct;
    direct.channel = ThreadChannel::E2e;
    direct.participant_contact_ids = {"contact-peer"};
    direct.peer_identity_kind = "account";
    direct.peer_identity_value = "account:peer";
    ASSERT_TRUE(store_.UpsertThread(direct));
    suppression_ = std::make_unique<AttachmentSuppressionStore>(dir_.string());

    workflow_.SetProfileDataDir(dir_.string());
    content_ = MakeInMemoryIndexedCasStore(dir_.string(), "test");
    workflow_.SetContent(content_);
    workflow_.SetAllowLocalCdnForTest(true);
    workflow_.SetFetchDependencies(&store_, contacts_.get(), identity_.get(), &peer_);
    workflow_.SetSuppressionStore(suppression_.get());
    ASSERT_TRUE(workflow_.SetDek(ByteVector(kDataEncryptionKeySize, 0x41)));
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
    workflow_.SetOnChanged([this]() { ++changed_; });

    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::shared_future<void> hold;
      std::string body;
      {
        std::lock_guard lock(mutex_);
        hold = holds_[request.target];
        body = bodies_[request.target];
      }
      if (hold.valid()) {
        hold.wait();
      }
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = body};
    });
  }
  void TearDown() override {
    AppRuntime::ResumeWorkers();
    AppRuntime::Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** A small picture from alice in t1, served by the loopback server at `/name`; its ciphertext in `cipher_out`. */
  ThreadMessage Message(const std::string& name, ChatAttachmentFields* fields_out, Bytes* cipher_out = nullptr) {
    Bytes plain(2'000, static_cast<uint8_t>(name.size()));
    plain[0] = static_cast<uint8_t>(name.front());
    auto prepared = PrepareChatAttachmentFromBytes(plain, name + ".bin", "", 200'000'000);
    EXPECT_TRUE(prepared);
    {
      std::lock_guard lock(mutex_);
      bodies_["/" + name] = std::string(prepared->ciphertext.begin(), prepared->ciphertext.end());
    }
    if (cipher_out != nullptr) {
      *cipher_out = prepared->ciphertext;
    }
    ChatAttachmentFields fields = prepared->fields;
    fields.url = server_.Origin() + "/" + name;
    ThreadMessage message;
    message.id = name;
    message.thread_id = "t1";
    message.sender_contact_id = "alice";
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    *fields_out = fields;
    return message;
  }
  std::promise<void> HoldAnswer(const std::string& name) {
    std::promise<void> release;
    std::lock_guard lock(mutex_);
    holds_["/" + name] = release.get_future().share();
    return release;
  }
  size_t Gets(const std::string& name) const {
    size_t count = 0;
    for (const auto& request : server_.Requests()) {
      count += request.target == "/" + name ? 1 : 0;
    }
    return count;
  }
  State StateOf(const ChatAttachmentFields& fields) const { return workflow_.StateFor("t1", fields); }
  bool Becomes(const ChatAttachmentFields& fields, const State state) {
    return WaitFor([&] {
      AppRuntime::RunUITasks();
      return StateOf(fields) == state;
    });
  }

  /**
   * Gives the peer's answer on a thread of the test's own, as the mesh pump would, and records what happened there.
   * `unblock` runs when the answer did not return in time (so a blocking answer can finish before the thread is
   * joined).
   */
  OnAnswerThread AnswerOnOwnThread(PeerAnswer answer, Roe<Bytes> value, const std::function<void()>& unblock = {}) {
    AppRuntime::RunUITasks();
    OnAnswerThread seen;
    seen.changed_before = changed_.load();
    std::atomic<bool> ui_task_posted{false};
    std::atomic<int> changed_after{0};
    auto done = std::async(std::launch::async, [&, answer = std::move(answer), value = std::move(value)]() mutable {
      answer(std::move(value));
      ui_task_posted = AppRuntime::HasPendingUITasks();
      changed_after = changed_.load();
    });
    seen.returned_in_time = done.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    if (!seen.returned_in_time && unblock) {
      unblock();
    }
    done.wait();
    seen.ui_task_posted = ui_task_posted.load();
    seen.changed_after = changed_after.load();
    return seen;
  }

  std::mutex mutex_;
  std::map<std::string, std::string> bodies_;
  std::map<std::string, std::shared_future<void>> holds_;
  ScriptedHttpServer server_; // after what its handler reads: stopped before those go
  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<ContactsStore> contacts_;
  InMemoryDeliveryStore store_;
  std::unique_ptr<AttachmentSuppressionStore> suppression_;
  std::shared_ptr<CasStore> content_;
  HeldPeer peer_;
  std::atomic<int> changed_{0};
  AttachmentFetchWorkflow workflow_;
};

// The peer cannot be reached: the server download runs on a worker. The answer's thread returns while the server
// still holds its answer, and posts nothing to the UI.
TEST_F(FetchThreadsTest, APeerFailureReturnsAtOnceAndTheServerDownloadRunsOnAWorker) {
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("pic", &fields);
  std::promise<void> server_answer = HoldAnswer("pic");
  workflow_.EnqueueFromMessage("t1", message);
  PeerAnswer answer = peer_.TakeAnswer();
  ASSERT_TRUE(answer) << "the download asked the peer first";

  bool released = false;
  const OnAnswerThread seen = AnswerOnOwnThread(std::move(answer), Error("amp link manager: dial timeout"), [&] {
    server_answer.set_value();
    released = true;
  });
  EXPECT_TRUE(seen.returned_in_time) << "the answer's thread ran the server download";
  EXPECT_FALSE(seen.ui_task_posted) << "the answer's thread posted to the UI";
  EXPECT_EQ(seen.changed_after, seen.changed_before);
  if (!released) {
    EXPECT_TRUE(WaitFor([&] { return Gets("pic") == 1u; })) << "the server download started on a worker";
    EXPECT_EQ(StateOf(fields), State::Downloading) << "and is still waiting for the server";
    server_answer.set_value();
  }
  ASSERT_TRUE(Becomes(fields, State::Ready)) << "state " << static_cast<int>(StateOf(fields));
  EXPECT_EQ(Gets("pic"), 1u);
}

// The peer sends the bytes: the size check, the decrypt and the file work run on a worker, not on the answer's
// thread. A pending copy of the same bytes is dropped by that work, so while the workers are paused it is still there.
TEST_F(FetchThreadsTest, APeerSuccessIsDecryptedOnAWorkerNotOnTheAnswersThread) {
  ChatAttachmentFields fields;
  Bytes cipher;
  const ThreadMessage message = Message("pic", &fields, &cipher);
  workflow_.EnqueueFromMessage("t1", message);
  PeerAnswer answer = peer_.TakeAnswer();
  ASSERT_TRUE(answer);
  // A direct push of the same bytes lands while the fetch is out: the finished fetch drops it.
  ASSERT_TRUE(SavePendingAttachmentCiphertext(dir_.string(), "t1", fields.content_hash, cipher, {}));

  AppRuntime::PauseWorkers();
  const OnAnswerThread seen = AnswerOnOwnThread(std::move(answer), cipher);
  EXPECT_TRUE(seen.returned_in_time);
  EXPECT_FALSE(seen.ui_task_posted);
  EXPECT_EQ(seen.changed_after, seen.changed_before);
  EXPECT_TRUE(AttachmentPendingCiphertextExists(dir_.string(), "t1", fields.content_hash))
      << "the answer's thread did the fetch's file work (and so its decrypt)";
  EXPECT_EQ(StateOf(fields), State::Downloading) << "not ready before a worker has run";
  AppRuntime::ResumeWorkers();

  ASSERT_TRUE(Becomes(fields, State::Ready));
  EXPECT_FALSE(AttachmentPendingCiphertextExists(dir_.string(), "t1", fields.content_hash));
  EXPECT_EQ(Gets("pic"), 0u) << "the peer's bytes, not the server's";
}

// The runtime stopped before the peer answered (quit): nothing is downloaded, the job fails as "not ready", and it is
// not counted as a spent server download, so a later look queues it again.
TEST_F(FetchThreadsTest, APeerAnswerAfterTheRuntimeStoppedFailsAsNotReadyAndSpendsNothing) {
  ChatAttachmentFields fields;
  const ThreadMessage message = Message("pic", &fields);
  workflow_.EnqueueFromMessage("t1", message);
  PeerAnswer answer = peer_.TakeAnswer();
  ASSERT_TRUE(answer);
  AppRuntime::Shutdown();

  (void)AnswerOnOwnThread(std::move(answer), Error("amp link manager: dial timeout"));
  EXPECT_EQ(Gets("pic"), 0u) << "no request reached the server";
  EXPECT_EQ(StateOf(fields), State::Failed);
  EXPECT_FALSE(suppression_->IsSuppressed("t1", fields.content_hash)) << "not held back as a spent download";
}

// The fetch itself, without a workflow: with no runtime the answer is "not ready", which reached no server; with the
// tests' inline switch the old continuation runs on the answer's thread.
TEST_F(FetchThreadsTest, WithoutARuntimeAFetchEndsNotReadyUnlessTheTestSwitchRunsItInline) {
  ChatAttachmentFields fields;
  (void)Message("pic", &fields);
  AttachmentFetchContext context;
  context.thread_id = "t1";
  context.store = &store_;
  context.contacts = contacts_.get();
  context.identity = identity_.get();
  context.peer_client = &peer_;
  context.profile_data_dir = dir_.string();
  context.allow_local_cdn_for_test = true;
  AppRuntime::Shutdown();

  std::optional<Roe<Bytes>> result;
  FetchAttachmentCiphertextAsync(fields, context, [&](Roe<Bytes> value) { result = std::move(value); });
  PeerAnswer answer = peer_.TakeAnswer();
  ASSERT_TRUE(answer);
  answer(Error("amp link manager: dial timeout"));
  ASSERT_TRUE(result.has_value());
  ASSERT_FALSE(*result);
  EXPECT_TRUE(IsAttachmentFetchNotReady(result->error())) << result->error().message;
  EXPECT_FALSE(FetchFailureReachedTheServer(result->error()));
  EXPECT_EQ(Gets("pic"), 0u);

  context.run_peer_answer_inline_for_test = true;
  result.reset();
  FetchAttachmentCiphertextAsync(fields, context, [&](Roe<Bytes> value) { result = std::move(value); });
  answer = peer_.TakeAnswer();
  ASSERT_TRUE(answer);
  answer(Error("amp link manager: dial timeout"));
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(*result) << "fetched from the server inline";
  EXPECT_EQ(Gets("pic"), 1u);
}

} // namespace
} // namespace pbr
