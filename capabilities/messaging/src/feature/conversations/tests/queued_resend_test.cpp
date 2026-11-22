// B236: a message of ours sent again as itself — after a restart (what was still queued) or by a tap on its red mark.
// The same message id, its copy sealed again in the device pair as a first send seals it: the recipient keeps it once
// (its receive pipeline drops an id it holds), and gets it when the first copy never arrived.
#include "feature/conversations/QueuedResend.h"

#include "common/runtime/AppRuntime.h"
#include "foundation/error/AppError.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/PeerCards.h"
#include "lib/people/PeerKemKeyStore.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/CopyAsk.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

ByteVector TestDek(const uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

/** One device: its account, stores and receive pipeline. */
struct Side {
  explicit Side(const uint8_t seed) {
    keys = test::TestAccount::Make();
    store = std::make_unique<InMemoryDeliveryStore>();
    EXPECT_TRUE(store->SetDek(TestDek(seed)) && store->ListThreads());
    roster = std::make_unique<InMemoryGroupRosterStore>(*store);
    psk = std::make_unique<InMemoryPeerSessions>(*store);
    EXPECT_TRUE(psk->SetDek(TestDek(seed)));
    pipeline = std::make_unique<RelayReceivePipeline>(
        *store,
        [this](const std::string& account) -> std::optional<AccountCard> {
          auto found = cards.find(account);
          return found == cards.end() ? std::nullopt : std::optional<AccountCard>(found->second);
        },
        *psk, test::ReceiveKeysOf(keys), *roster);
  }
  ~Side() {
    pipeline.reset();
    psk.reset();
    roster.reset();
    store.reset();
  }

  void Know(const Side& peer) { cards[peer.keys.account_id] = peer.keys.DeviceCard(); }

  Thread PublicChatWith(const Side& peer) {
    DirectChatTarget target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.keys.account_id;
    target.channel = ThreadChannel::E2ePublic;
    auto thread = store->FindOrCreateDirectThread(target, "", "peer");
    EXPECT_TRUE(thread);
    return *thread;
  }

  /** Our message to `peer`, stored as the composer stores it (pending). */
  ThreadMessage Store(const Thread& thread, const std::string& text) {
    auto appended = AppendOutboundMessage(
        *store, NewOutboundMessage(thread.id, text, SendRelayOptions{}, util::GenerateUuid(), util::NowUnixMs()));
    EXPECT_TRUE(appended);
    return *appended;
  }

  /**
   * The stored row `message` as it goes to `peer`'s device: sealed in the device pair (numbered there now) and signed,
   * as MeshDeliveryOrchestrator::DispatchDirect builds it — with ResendStoredMessage's options when `resend`.
   */
  RelayEnvelope Seal(const ThreadMessage& message, const Side& peer, const bool resend) {
    SendRelayOptions options;
    if (resend) {
      options = copy_ask::ResendOptions(message); // wakes nobody (P022)
    }
    const AccountCardDevice device = peer.keys.device.Entry();
    const PeerSessionKey pair = GroupE2ePayloadCodec::PairTargetKey(peer.keys.account_id, device.peer_id);
    auto copy =
        SealPairCopy(message, keys.account_id, options, *psk, pair, Base64Decode(device.kem_public_key_b64).value());
    EXPECT_TRUE(copy) << copy.error().message;
    RelayEnvelope envelope = BuildDirectEnvelope(
        message, options, EnvelopeSender{keys.account_id, keys.device.peer_id, std::nullopt}, ThreadChannel::E2ePublic,
        "account:" + peer.keys.account_id, {DeviceCopy(device.peer_id, std::move(*copy))}, NextRelayOrderKey());
    test::SignEnvelopeAs(keys, envelope);
    return envelope;
  }

  RelayReceiveOutcome Receive(const RelayEnvelope& envelope) {
    return pipeline->ProcessEnvelope(envelope, keys.account_id);
  }

  /** How many rows with `id` the chat with `peer` holds. */
  size_t Rows(const Side& peer, const std::string& id) {
    DirectChatTarget target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.keys.account_id;
    target.channel = ThreadChannel::E2ePublic;
    auto thread = store->FindDirectThread(target);
    if (!thread || !*thread) {
      return 0;
    }
    auto rows = store->GetMessagesPage((*thread)->id, std::nullopt, 1000);
    size_t count = 0;
    for (const ThreadMessage& row : rows ? *rows : std::vector<ThreadMessage>{}) {
      count += row.id == id ? 1 : 0;
    }
    return count;
  }

  test::TestAccount keys;
  std::map<std::string, AccountCard> cards;
  std::unique_ptr<InMemoryDeliveryStore> store;
  std::unique_ptr<InMemoryGroupRosterStore> roster;
  std::unique_ptr<InMemoryPeerSessions> psk;
  std::unique_ptr<RelayReceivePipeline> pipeline;
};

class QueuedResendTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    alice_ = std::make_unique<Side>(0x10);
    bob_ = std::make_unique<Side>(0x40);
    alice_->Know(*bob_);
    bob_->Know(*alice_);
  }

  std::unique_ptr<Side> alice_;
  std::unique_ptr<Side> bob_;
};

// The relay took the first copy but its answer never reached us (the app was suspended): the message stayed queued, and
// after the restart it goes again as itself. Bob keeps it once; what Alice sends next is read as usual.
TEST_F(QueuedResendTest, AMessageSentAgainAsItselfIsKeptOnceByTheRecipient) {
  const Thread chat = alice_->PublicChatWith(*bob_);
  const ThreadMessage photo = alice_->Store(chat, "photo.jpg");
  const RelayReceiveOutcome first = bob_->Receive(alice_->Seal(photo, *bob_, false));
  ASSERT_TRUE(first.persisted);

  const RelayReceiveOutcome again = bob_->Receive(alice_->Seal(photo, *bob_, true));
  EXPECT_FALSE(again.persisted) << "the same message id: not kept twice";
  EXPECT_EQ(again.decision, IngestDecision::BenignDuplicate);
  EXPECT_FALSE(again.receive_failure) << "nothing went wrong on Bob's side";
  // The very same envelope twice (a relay retry that went through twice) as well.
  const RelayEnvelope same = alice_->Seal(photo, *bob_, true);
  EXPECT_FALSE(bob_->Receive(same).persisted);
  EXPECT_FALSE(bob_->Receive(same).persisted);
  EXPECT_EQ(bob_->Rows(*alice_, photo.id), 1u);

  const ThreadMessage next = alice_->Store(chat, "and this");
  EXPECT_TRUE(bob_->Receive(alice_->Seal(next, *bob_, false)).persisted)
      << "the numbers the dropped copies used leave no hole that stops the chat";
  EXPECT_EQ(bob_->Rows(*alice_, next.id), 1u);
}

// The first copy never reached the relay (refused, the app stopped): the copy sent again after the restart is read.
TEST_F(QueuedResendTest, AFirstCopyThatNeverArrivedIsReadFromTheOneSentAgain) {
  const Thread chat = alice_->PublicChatWith(*bob_);
  const ThreadMessage text = alice_->Store(chat, "hello");
  (void)alice_->Seal(text, *bob_, false); // lost on the way
  const RelayReceiveOutcome again = bob_->Receive(alice_->Seal(text, *bob_, true));
  ASSERT_TRUE(again.persisted);
  ASSERT_TRUE(again.persisted_message);
  EXPECT_EQ(again.persisted_message->id, text.id);
  EXPECT_EQ(again.persisted_message->text, "hello");
  EXPECT_EQ(bob_->Rows(*alice_, text.id), 1u);
  EXPECT_TRUE(bob_->Receive(alice_->Seal(alice_->Store(chat, "next"), *bob_, false)).persisted);
}

// B236 R3: the copy dropped by its id is no new message for the recipient — nothing is stored, so the receive paths
// (MeshDeliveryOrchestrator: OnInboundMessagePersisted, the inbound and background notices) raise no unread count and
// no notice: each of them runs only for a persisted outcome.
TEST_F(QueuedResendTest, TheCopyDroppedByItsIdIsNoNewMessageForTheRecipient) {
  const Thread chat = alice_->PublicChatWith(*bob_);
  const ThreadMessage text = alice_->Store(chat, "hi");
  ASSERT_TRUE(bob_->Receive(alice_->Seal(text, *bob_, false)).persisted);
  const RelayReceiveOutcome again = bob_->Receive(alice_->Seal(text, *bob_, true));
  EXPECT_FALSE(again.persisted);
  EXPECT_FALSE(again.persisted_message);
  EXPECT_FALSE(again.thread_changed);
  EXPECT_FALSE(again.receive_failure);
  EXPECT_FALSE(again.missing_copy) << "no copy asked for either";
}

// B236 R2 (open, see the report): the copy sent again is numbered anew in its device pair, so the number the first
// copy (or the dropped one) used is a hole on the recipient's side: the next message is accepted, but as past a gap,
// and the pair's contiguous count stays below the hole. Nothing is lost or held back; what remains is the repair.
TEST_F(QueuedResendTest, TheNumberADroppedCopyUsedStaysAHoleTheNextMessagesAreReadPastIt) {
  const Thread chat = alice_->PublicChatWith(*bob_);
  const ThreadMessage first = alice_->Store(chat, "one");
  ASSERT_TRUE(bob_->Receive(alice_->Seal(first, *bob_, false)).persisted);
  ASSERT_FALSE(bob_->Receive(alice_->Seal(first, *bob_, true)).persisted);
  const RelayReceiveOutcome next = bob_->Receive(alice_->Seal(alice_->Store(chat, "two"), *bob_, false));
  EXPECT_TRUE(next.persisted);
  EXPECT_EQ(next.decision, IngestDecision::AcceptGap) << "past the hole the dropped copy left";
  const RelayReceiveOutcome after = bob_->Receive(alice_->Seal(alice_->Store(chat, "three"), *bob_, false));
  EXPECT_TRUE(after.persisted);
  EXPECT_EQ(after.decision, IngestDecision::AcceptGap) << "the hole is not closed by later messages";
}

ThreadMessage Ours(const std::string& id, const MessageDelivery delivery, const int64_t at,
                   const ChatContentType type = ChatContentType::Text) {
  ThreadMessage message;
  message.id = id;
  message.sender_contact_id = kLocalSelfContactId;
  message.content_type = type;
  message.text = id;
  message.timestamp = at;
  message.delivery = delivery;
  message.relay_visible = true;
  return message;
}

TEST(QueuedResendRulesTest, WhatIsSentAgainAsItself) {
  Thread public_chat;
  public_chat.kind = ThreadKind::Direct;
  public_chat.channel = ThreadChannel::E2ePublic;
  Thread private_chat = public_chat;
  private_chat.channel = ThreadChannel::E2e;
  Thread group;
  group.kind = ThreadKind::Group;
  group.group_id = "g1";
  Thread ai;
  ai.kind = ThreadKind::Ai;

  EXPECT_TRUE(StoredResendAllowed(public_chat, Ours("m", MessageDelivery::Failed, 1)));
  for (const ChatContentType type :
       {ChatContentType::Attachment, ChatContentType::ContactCard, ChatContentType::CryptoTx}) {
    EXPECT_TRUE(StoredResendAllowed(public_chat, Ours("m", MessageDelivery::Failed, 1, type)));
    EXPECT_TRUE(StoredResendAllowed(group, Ours("m", MessageDelivery::Pending, 1, type)));
  }
  EXPECT_FALSE(StoredResendAllowed(private_chat, Ours("m", MessageDelivery::Failed, 1))) << "strict numbering";
  EXPECT_FALSE(StoredResendAllowed(ai, Ours("m", MessageDelivery::Failed, 1)));
  EXPECT_FALSE(StoredResendAllowed(public_chat, Ours("m", MessageDelivery::Pending, 1, ChatContentType::System)));
  EXPECT_FALSE(StoredResendAllowed(public_chat, Ours("m", MessageDelivery::Pending, 1, ChatContentType::Annotation)));
  ThreadMessage theirs = Ours("m", MessageDelivery::Relayed, 1);
  theirs.sender_contact_id = "account:bob";
  EXPECT_FALSE(StoredResendAllowed(public_chat, theirs));
  ThreadMessage siblings = Ours("m", MessageDelivery::Pending, 1);
  siblings.sender_device = "QmOtherDeviceOfOurs";
  EXPECT_FALSE(StoredResendAllowed(public_chat, siblings)) << "our other device's to send";
  ThreadMessage local = Ours("m", MessageDelivery::Pending, 1);
  local.relay_visible = false;
  EXPECT_FALSE(StoredResendAllowed(public_chat, local));
}

// After a restart: what was still queued goes again (once), what has waited too long is given up, and nothing else is
// touched — a row the old app marked failed stays as it is (no migration).
TEST(QueuedResendRulesTest, AfterARestartTheQueuedRowsGoAgainOnce) {
  InMemoryDeliveryStore store;
  ASSERT_TRUE(store.SetDek(TestDek(1)));
  Thread chat;
  chat.id = "t1";
  chat.kind = ThreadKind::Direct;
  chat.channel = ThreadChannel::E2ePublic;
  chat.peer_identity_kind = "account";
  chat.peer_identity_value = "account:bob";
  ASSERT_TRUE(store.UpsertThread(chat));
  Thread secret = chat;
  secret.id = "t2";
  secret.channel = ThreadChannel::E2e;
  ASSERT_TRUE(store.UpsertThread(secret));
  const int64_t now = 10LL * 24 * 60 * 60 * 1000;
  auto add = [&store](const std::string& thread_id, ThreadMessage message) {
    message.thread_id = thread_id;
    ASSERT_TRUE(store.AppendMessage(message));
  };
  add("t1", Ours("queued", MessageDelivery::Pending, now - 60'000, ChatContentType::Attachment));
  add("t1", Ours("queued-text", MessageDelivery::Pending, now - 1000));
  add("t1", Ours("old", MessageDelivery::Pending, now - kResumeQueuedMaxAgeMs - 1));
  add("t1", Ours("failed-before", MessageDelivery::Failed, now - 60'000));
  add("t1", Ours("delivered", MessageDelivery::Relayed, now - 60'000));
  add("t1", Ours("system", MessageDelivery::Pending, now - 60'000, ChatContentType::System));
  add("t2", Ours("private", MessageDelivery::Pending, now - 60'000));

  add("t1", Ours("this-process", MessageDelivery::Pending, now + 10)); // sent since the start: by its own send
  const QueuedResumePlan plan = PlanQueuedResume(store, now + 20, now, [](const std::string&) { return false; });
  EXPECT_EQ(plan.resend, (std::vector<QueuedRow>{{"t1", "queued"}, {"t1", "queued-text"}}));
  EXPECT_EQ(plan.give_up, (std::vector<QueuedRow>{{"t1", "old"}}));

  // A resume while the first one's sends are on their way plans nothing again: each goes once.
  const QueuedResumePlan second = PlanQueuedResume(
      store, now + 20, now, [](const std::string& id) { return id == "queued" || id == "queued-text"; });
  EXPECT_TRUE(second.resend.empty());
}

// --- ResendStoredMessage / ResumeQueuedSends on a real MeshDeliveryOrchestrator ------------------------------------

/** A relay that records each send; held, a send waits until released; `fail`, it is refused for good (400). */
class RecordingRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::unique_lock lock(mu_);
    sent_.push_back(envelope);
    entered_.notify_all();
    released_.wait(lock, [this] { return !held_; });
    if (fail_) {
      return AppError::Network(Err::Network::HttpRejected, "400");
    }
    return {};
  }
  void Hold() {
    std::lock_guard lock(mu_);
    held_ = true;
  }
  void Release() {
    std::lock_guard lock(mu_);
    held_ = false;
    released_.notify_all();
  }
  void SetFail(const bool fail) {
    std::lock_guard lock(mu_);
    fail_ = fail;
  }
  bool WaitForSends(const size_t count) {
    std::unique_lock lock(mu_);
    return entered_.wait_for(lock, std::chrono::seconds(5), [&] { return sent_.size() >= count; });
  }
  std::vector<RelayEnvelope> Sent() {
    std::lock_guard lock(mu_);
    return sent_;
  }
  size_t SentWithId(const std::string& id) {
    std::lock_guard lock(mu_);
    return static_cast<size_t>(
        std::count_if(sent_.begin(), sent_.end(), [&](const RelayEnvelope& e) { return e.message_id == id; }));
  }
  /** What the next poll hands over (once). */
  void Deliver(RelayEnvelope envelope) {
    std::lock_guard lock(mu_);
    inbox_.push_back(std::move(envelope));
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override {
    std::lock_guard lock(mu_);
    RelayPollResult result;
    result.messages.swap(inbox_);
    result.next_cursor = "c" + std::to_string(++polls_);
    return result;
  }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return RelayDeleteResult{}; }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  /** Stream history (a gap repair asks it): always an empty answer, as a sender that holds nothing in the range. */
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override {
    std::lock_guard lock(mu_);
    history_asks_.push_back(request);
    ChatHistoryResponse response;
    response.session_epoch = request.session_epoch;
    return response;
  }
  std::vector<ChatHistoryRequest> HistoryAsks() {
    std::lock_guard lock(mu_);
    return history_asks_;
  }

private:
  std::vector<RelayEnvelope> inbox_;
  std::vector<ChatHistoryRequest> history_asks_;
  int polls_ = 0;
  std::mutex mu_;
  std::condition_variable entered_;
  std::condition_variable released_;
  std::vector<RelayEnvelope> sent_;
  bool held_ = false;
  bool fail_ = false;
};

/** Alice's app as the hub builds it, minus the mesh: its stores, its identity, and the orchestrator on top. */
class StoredResendTest : public ::testing::Test {
protected:
  StoredResendTest()
      : dir_(std::filesystem::temp_directory_path() / ("pp_stored_resend_" + util::GenerateUuid())),
        identity_(dir_.string(), "test"),
        psk_(store_),
        roster_(store_),
        contacts_(dir_.string()),
        shadows_(directory_),
        labels_(contacts_, PeerDisplaySourcesFrom(shadows_)),
        inbox_(store_, contacts_, labels_, &shadows_),
        cards_(card_rows_),
        keys_(signing_, kem_, cards_, directory_) {}

  void SetUp() override {
    EnsureSodiumInit();
    std::filesystem::create_directories(dir_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(identity_.SetDek(TestDek(3)) && psk_.SetDek(TestDek(3)) && store_.SetDek(TestDek(3)));
    ASSERT_TRUE(identity_.LoadOrCreate());
    bob_ = test::TestAccount::Make();
    ASSERT_TRUE(keys_.AddAccountCard(bob_.DeviceCard(), util::NowUnixMs(), "test"));
    relay_ = std::make_shared<RecordingRelay>();
    mesh_ =
        std::make_unique<MeshDeliveryOrchestrator>(store_, contacts_, identity_, relay_, inbox_, keys_, psk_, roster_);
    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = bob_.account_id;
    target.channel = ThreadChannel::E2ePublic;
    auto thread = store_.FindOrCreateDirectThread(target, "", "Bob");
    ASSERT_TRUE(thread);
    chat_ = *thread;
  }

  void TearDown() override {
    relay_->Release();
    (void)AppRuntime::DrainWorkersThenUI();
    mesh_.reset();
    AppRuntime::Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** A message of ours in `thread_id`, stored as an earlier run of the app left it. */
  void Add(const std::string& thread_id, const std::string& id, const MessageDelivery delivery,
           const int64_t age_ms = 60'000, const ChatContentType type = ChatContentType::Text) {
    ThreadMessage message = Ours(id, delivery, util::NowUnixMs() - age_ms, type);
    message.thread_id = thread_id;
    ASSERT_TRUE(store_.AppendMessage(message));
  }

  MessageDelivery Delivery(const std::string& thread_id, const std::string& id) {
    auto row = store_.GetMessage(thread_id, id);
    return row && *row ? (*row)->delivery : MessageDelivery::Local;
  }

  void Drain() { ASSERT_TRUE(AppRuntime::DrainWorkersThenUI()); }

  Thread MakeGroup(const GroupSyncState state) {
    auto thread = store_.FindOrCreateGroupThread("g1", "Group", {});
    EXPECT_TRUE(thread);
    GroupMetadata metadata;
    metadata.group_id = "g1";
    metadata.owner_identity = identity_.Get()->account_id;
    metadata.sync_state = state;
    EXPECT_TRUE(roster_.UpsertMetadata(metadata));
    for (const std::string& member : {identity_.Get()->account_id, bob_.account_id}) {
      GroupRosterMember entry;
      entry.member_identity = member;
      EXPECT_TRUE(roster_.UpsertMember("g1", entry));
    }
    return *thread;
  }

  std::filesystem::path dir_;
  InMemoryDeliveryStore store_;
  IdentityStore identity_;
  InMemoryPeerSessions psk_;
  InMemoryGroupRosterStore roster_;
  ContactsStore contacts_;
  MockDirectoryClient directory_;
  DirectoryShadowCache shadows_;
  PeerDisplayResolver labels_;
  InboxController inbox_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  InMemoryPeerCardStore card_rows_;
  PeerCards cards_;
  AccountKeyResolver keys_;
  test::TestAccount bob_;
  std::shared_ptr<RecordingRelay> relay_;
  std::unique_ptr<MeshDeliveryOrchestrator> mesh_;
  Thread chat_;
};

TEST_F(StoredResendTest, AFailedMessageGoesAgainAsItselfAndIsMarkedSent) {
  Add(chat_.id, "m1", MessageDelivery::Failed);
  auto sent = mesh_->ResendStoredMessage(chat_.id, "m1");
  ASSERT_TRUE(sent) << sent.error().message;
  EXPECT_TRUE(*sent);
  Drain();
  ASSERT_EQ(relay_->SentWithId("m1"), 1u) << "the same message id";
  EXPECT_EQ(relay_->Sent().front().recipient_account_id.value_or(""), bob_.account_id);
  EXPECT_TRUE(relay_->Sent().front().relay_wake_hint.empty()) << "P022: a re-sent copy wakes nobody";
  EXPECT_EQ(Delivery(chat_.id, "m1"), MessageDelivery::Relayed);
  auto again = mesh_->ResendStoredMessage(chat_.id, "m1");
  EXPECT_FALSE(again) << "delivered: never sent again";
  EXPECT_EQ(relay_->SentWithId("m1"), 1u);
}

TEST_F(StoredResendTest, ASecondResendWhileTheFirstIsOnItsWaySendsNothing) {
  Add(chat_.id, "m1", MessageDelivery::Failed);
  relay_->Hold();
  ASSERT_TRUE(mesh_->ResendStoredMessage(chat_.id, "m1"));
  ASSERT_TRUE(relay_->WaitForSends(1));
  EXPECT_EQ(Delivery(chat_.id, "m1"), MessageDelivery::Pending) << "queued again before it went";
  auto second = mesh_->ResendStoredMessage(chat_.id, "m1");
  ASSERT_TRUE(second);
  EXPECT_FALSE(*second) << "still under way: nothing done";
  relay_->Release();
  Drain();
  EXPECT_EQ(relay_->SentWithId("m1"), 1u);
}

TEST_F(StoredResendTest, ADispatchThatFailsAfterThePendingWriteFailsTheRowAgain) {
  DirectChatTarget nowhere;
  nowhere.peer_identity_kind = "account";
  nowhere.peer_identity_value = "not-an-account";
  nowhere.channel = ThreadChannel::E2ePublic;
  auto thread = store_.FindOrCreateDirectThread(nowhere, "", "Nobody");
  ASSERT_TRUE(thread);
  Add(thread->id, "m1", MessageDelivery::Failed);
  auto sent = mesh_->ResendStoredMessage(thread->id, "m1");
  EXPECT_FALSE(sent) << "no Account ID, no link: nothing to hand over";
  EXPECT_EQ(Delivery(thread->id, "m1"), MessageDelivery::Failed) << "not left queued";
  Drain();
  EXPECT_TRUE(relay_->Sent().empty());
}

TEST_F(StoredResendTest, AGroupMessageGoesAgainToItsMembers) {
  const Thread group = MakeGroup(GroupSyncState::Active);
  Add(group.id, "g-m1", MessageDelivery::Failed);
  auto sent = mesh_->ResendStoredMessage(group.id, "g-m1");
  ASSERT_TRUE(sent) << sent.error().message;
  Drain();
  ASSERT_EQ(relay_->SentWithId("g-m1"), 1u) << "one record, to the one other member";
  EXPECT_EQ(relay_->Sent().front().recipient_account_id.value_or(""), bob_.account_id);
  EXPECT_EQ(Delivery(group.id, "g-m1"), MessageDelivery::Relayed);
}

TEST_F(StoredResendTest, AGroupWeWereRemovedFromOrStillCheckingSendsNothing) {
  for (const auto& [state, error] : {std::pair{GroupSyncState::Removed, group_roster_sync::kGroupRemovedError},
                                     std::pair{GroupSyncState::Checking, group_roster_sync::kGroupCheckingError}}) {
    const Thread group = MakeGroup(state);
    const std::string id = std::string("g-") + error;
    Add(group.id, id, MessageDelivery::Failed);
    auto sent = mesh_->ResendStoredMessage(group.id, id);
    ASSERT_FALSE(sent);
    EXPECT_EQ(sent.error().message, error);
    EXPECT_EQ(Delivery(group.id, id), MessageDelivery::Failed) << "left as it was";
  }
  Drain();
  EXPECT_TRUE(relay_->Sent().empty());
}

// R7 + the resume: what was still queued shows queued from the first frame, goes once at the first sync, and a tap
// racing the resume makes no second copy.
TEST_F(StoredResendTest, AfterAStartTheQueuedRowsShowQueuedThenGoOnceEvenWithATapRacing) {
  Add(chat_.id, "q1", MessageDelivery::Pending);
  Add(chat_.id, "q2", MessageDelivery::Pending, 120'000, ChatContentType::Attachment);
  Add(chat_.id, "old", MessageDelivery::Pending, kResumeQueuedMaxAgeMs + 60'000);
  Add(chat_.id, "failed-by-old-app", MessageDelivery::Failed);
  mesh_->PrimeQueuedResume();
  EXPECT_TRUE(mesh_->SendStartedMs("q1")) << "queued, not red, before the first sync";
  EXPECT_TRUE(mesh_->SendStartedMs("q2"));
  EXPECT_FALSE(mesh_->SendStartedMs("old"));
  EXPECT_EQ(Delivery(chat_.id, "old"), MessageDelivery::Failed) << "too old: red at once";
  EXPECT_FALSE(mesh_->SendStartedMs("failed-by-old-app"));
  EXPECT_EQ(Delivery(chat_.id, "failed-by-old-app"), MessageDelivery::Failed) << "left as it was";

  relay_->Hold();
  std::thread resume([this]() { mesh_->ResumeQueuedSends(); });
  std::thread tap([this]() { (void)mesh_->ResendStoredMessage(chat_.id, "q1"); });
  resume.join();
  tap.join();
  mesh_->ResumeQueuedSends(); // a second sync: nothing again
  relay_->Release();
  Drain();
  EXPECT_EQ(relay_->SentWithId("q1"), 1u) << "the tap and the resume: one copy";
  for (const RelayEnvelope& envelope : relay_->Sent()) {
    EXPECT_TRUE(envelope.relay_wake_hint.empty()) << "the start's resend wakes nobody either (P022)";
  }
  EXPECT_EQ(relay_->SentWithId("q2"), 1u);
  EXPECT_EQ(relay_->SentWithId("old"), 0u);
  EXPECT_EQ(relay_->SentWithId("failed-by-old-app"), 0u);
  EXPECT_EQ(Delivery(chat_.id, "q1"), MessageDelivery::Relayed);
  EXPECT_EQ(Delivery(chat_.id, "q2"), MessageDelivery::Relayed);
  EXPECT_FALSE(mesh_->SendStartedMs("q1"));
}

TEST_F(StoredResendTest, AResumedSendRefusedForGoodEndsRed) {
  Add(chat_.id, "q1", MessageDelivery::Pending);
  relay_->SetFail(true);
  mesh_->ResumeQueuedSends();
  Drain();
  EXPECT_EQ(Delivery(chat_.id, "q1"), MessageDelivery::Failed);
  EXPECT_FALSE(mesh_->SendStartedMs("q1")) << "nothing of it is tried any more: the red mark";
}

// B236 R2, evidence for the transport owner (no product change): a device-pair copy that never arrives (here: lost on
// the way) leaves a hole in that pair's numbering on the recipient. Driven through the real relay poll path of the
// recipient's orchestrator: one copy, the hole, then five genuine messages, the sender answering every history ask
// empty. Recorded: the decision for each message, the history asks the recipient makes (MaybeRepairGap), and whether
// the empty answers close the pair's hole.
TEST_F(StoredResendTest, ADevicePairHoleIsAskedAboutOnEveryLaterMessageAndNeverCloses) {
  const LocalIdentity me = *identity_.Get();
  mesh_->SetProfileDataDir(dir_.string());
  mesh_->SetRelayClients(MeshDeliveryOrchestrator::RelayClients{
      .preferred = relay_, .history = relay_, .sender = relay_.get(), .inboxes = {RelayInbox{"https://r", relay_}}});
  // Carol, a peer whose card we keep, writes to this device in her pair with it.
  test::TestAccount carol = test::TestAccount::Make();
  ASSERT_TRUE(keys_.AddAccountCard(carol.DeviceCard(), util::NowUnixMs(), "test"));
  InMemoryDeliveryStore carol_store;
  ASSERT_TRUE(carol_store.SetDek(TestDek(9)));
  InMemoryPeerSessions carol_psk(carol_store);
  ASSERT_TRUE(carol_psk.SetDek(TestDek(9)));
  const PeerSessionKey pair = GroupE2ePayloadCodec::PairTargetKey(me.account_id, me.peer_id);
  const ByteVector my_device_kem = Base64Decode(me.device_kem_public_key_b64).value();
  auto seal = [&](const std::string& text) {
    ThreadMessage message;
    message.id = util::GenerateUuid();
    message.text = text;
    message.timestamp = util::NowUnixMs();
    auto copy = SealPairCopy(message, carol.account_id, SendRelayOptions{}, carol_psk, pair, my_device_kem);
    EXPECT_TRUE(copy) << copy.error().message;
    RelayEnvelope envelope = BuildDirectEnvelope(
        message, SendRelayOptions{}, EnvelopeSender{carol.account_id, carol.device.peer_id, std::nullopt},
        ThreadChannel::E2ePublic, me.account_id, {DeviceCopy(me.peer_id, std::move(*copy))}, NextRelayOrderKey());
    test::SignEnvelopeAs(carol, envelope);
    return envelope;
  };
  auto poll = [&](RelayEnvelope envelope) {
    relay_->Deliver(std::move(envelope));
    mesh_->SyncInboxFromWake(true);
    Drain();
    Drain(); // the repair the ingest started, on its own worker turn
  };

  poll(seal("one"));  // seq 1
  (void)seal("lost"); // seq 2: never reaches us
  DirectChatTarget from_carol;
  from_carol.peer_identity_kind = "account";
  from_carol.peer_identity_value = carol.account_id;
  from_carol.channel = ThreadChannel::E2ePublic;
  auto thread = store_.FindDirectThread(from_carol);
  ASSERT_TRUE(thread && *thread) << "the first message made the chat";
  const size_t asks_before = relay_->HistoryAsks().size();
  std::vector<size_t> asks_after_each;
  for (int n = 1; n <= 5; ++n) {
    poll(seal("genuine " + std::to_string(n))); // seq 3..7
    asks_after_each.push_back(relay_->HistoryAsks().size() - asks_before);
  }
  auto rows = store_.GetMessagesPage((*thread)->id, std::nullopt, 100);
  ASSERT_TRUE(rows);
  EXPECT_EQ(rows->size(), 6u) << "every genuine message is read (nothing held back by the hole)";

  // What the pair's numbering holds after the empty answers.
  const uint32_t epoch = 1;
  auto pair_state = store_.GetPeerSyncState((*thread)->id, epoch, carol.device.peer_id);
  ASSERT_TRUE(pair_state);
  const std::vector<ChatHistoryRequest> asks = relay_->HistoryAsks();
  std::string ranges;
  for (const ChatHistoryRequest& ask : asks) {
    ranges += " [" + std::to_string(ask.min_sender_seq.value_or(0)) + "," +
              std::to_string(ask.max_sender_seq.value_or(0)) + "]";
  }
  RecordProperty("history_asks_after_each_message",
                 std::to_string(asks_after_each[0]) + "," + std::to_string(asks_after_each[1]) + "," +
                     std::to_string(asks_after_each[2]) + "," + std::to_string(asks_after_each[3]) + "," +
                     std::to_string(asks_after_each[4]));
  std::cout << "[ R2 evidence ] history asks after each of 5 messages (cumulative): " << asks_after_each[0] << ","
            << asks_after_each[1] << "," << asks_after_each[2] << "," << asks_after_each[3] << "," << asks_after_each[4]
            << "; ranges:" << ranges << "; pair contiguous=" << pair_state->contiguous_peer_seq
            << " phase=" << static_cast<int>(pair_state->phase) << " loaded_max=" << pair_state->loaded_max_seq
            << std::endl;
  // Today's behaviour, as observed (the report files it): one ask per later message, and the pair's hole stays open.
  EXPECT_EQ(asks_after_each.back(), 5u) << "one history ask per message after the hole";
  EXPECT_EQ(pair_state->contiguous_peer_seq, 1u) << "the empty answers do not close the device pair's hole";
  EXPECT_EQ(pair_state->phase, PeerSyncPhase::Gap);
}

} // namespace
} // namespace pbr
