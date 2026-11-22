// B236: a message of ours sent again as itself — after a restart (what was still queued) or by a tap on its red mark.
// The same message id, its copy sealed again in the device pair as a first send seals it: the recipient keeps it once
// (its receive pipeline drops an id it holds), and gets it when the first copy never arrived.
#include "feature/conversations/QueuedResend.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/CardKeys.h"
#include "lib/messaging/CopyAsk.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

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
      options = copy_ask::ResendOptions(message);
      options.wake.reset();
    }
    const AccountCardDevice device = peer.keys.device.Entry();
    const PeerSessionKey pair = GroupE2ePayloadCodec::PairTargetKey(peer.keys.account_id, device.peer_id);
    auto copy = SealPairCopy(message, keys.account_id, options, *psk, pair,
                             RawKemPublicKey(device.kem_public_key_b64, "the device KEM key").value());
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

} // namespace
} // namespace pbr
