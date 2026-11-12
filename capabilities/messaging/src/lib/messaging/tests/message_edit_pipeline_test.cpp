// Editing a sent message (DEV/PLAN-message-edit.md) through the receive pipeline on the paths the private-chat cases in
// messaging_cross_cutting_test.cpp do not take: a group, our own other devices (sent_copy on the control lane), the
// public channel, and a resent edit. Every envelope is built and sealed by the product's own send code (OutboundMessage,
// GroupE2ePayloadCodec, BuildControlLaneEnvelope, sent_copy) and read by a real RelayReceivePipeline.
#include "lib/delivery/AutoKeyEstablishment.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/EditMessage.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/SentCopy.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "common/directory/DirectoryJson.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "common/MeshAlias.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int64_t kMin = 60'000;

ByteVector TestDek(const uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

/** A text message, sent at `at`. */
ThreadMessage Text(const std::string& id, const std::string& text, const int64_t at) {
  ThreadMessage message;
  message.id = id;
  message.text = text;
  message.timestamp = at;
  return message;
}

/** An edit of `target_id` to `text`, sent at `at`, as EditSendOptions puts it on the wire. */
ThreadMessage Edit(const std::string& id, const std::string& target_id, const std::string& text, const int64_t at) {
  ThreadMessage edit = Text(id, text, at);
  edit.content_type = ChatContentType::Annotation;
  edit.target_message_id = target_id;
  edit.payload_json = BuildEditPayloadJson(target_id, text);
  return edit;
}

/** A delete for everyone of `target_id`, sent at `at`, as SendRetract puts it on the wire. */
ThreadMessage Retract(const std::string& id, const std::string& target_id, const int64_t at) {
  ThreadMessage retract = Text(id, "", at);
  retract.content_type = ChatContentType::Annotation;
  retract.target_message_id = target_id;
  retract.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, target_id, "");
  return retract;
}

/**
 * The send queue as far as these cases need it (DeliveryOutbox): offline, a send is queued by its message id; Cancel
 * drops a message's queued sends; back online, Retry sends what is queued, in order. `sent` is what went out.
 */
struct FakeOutbox {
  void Send(const std::string& message_id, const RelayEnvelope& envelope) {
    if (online) {
      sent.push_back(envelope);
    } else {
      queued.emplace_back(message_id, envelope);
    }
  }
  void Cancel(const std::string& message_id) {
    std::erase_if(queued, [&](const auto& entry) { return entry.first == message_id; });
  }
  void Retry() {
    online = true;
    for (const auto& entry : queued) {
      sent.push_back(entry.second);
    }
    queued.clear();
  }
  std::vector<std::string> SentIds() const {
    std::vector<std::string> ids;
    for (const RelayEnvelope& envelope : sent) {
      ids.push_back(envelope.message_id);
    }
    return ids;
  }

  bool online = false;
  std::vector<std::pair<std::string, RelayEnvelope>> queued;
  std::vector<RelayEnvelope> sent;
};

/**
 * The in-memory store, counting the full reads of a message's annotation rows (GetAnnotationsForTarget: each row
 * decrypted, in the app's store) and failing the append of message `fail_append_once` once.
 */
class ProbedStore final : public InMemoryDeliveryStore {
public:
  Roe<std::vector<ThreadMessage>> GetAnnotationsForTarget(const std::string& thread_id,
                                                          const std::string& target_id) const override {
    ++annotation_reads;
    return IThreadTranscript::GetAnnotationsForTarget(thread_id, target_id);
  }
  /** A count, not a read of the rows (the app's store counts them without decrypting). */
  Roe<int64_t> CountAnnotationsForTarget(const std::string& thread_id, const std::string& target_id) const override {
    auto rows = IThreadTranscript::GetAnnotationsForTarget(thread_id, target_id);
    return rows ? Roe<int64_t>(static_cast<int64_t>(rows->size())) : Roe<int64_t>(rows.error());
  }
  Roe<ThreadMessage> AppendMessage(const ThreadMessage& message) override {
    if (!fail_append_once.empty() && message.id == fail_append_once) {
      fail_append_once.clear();
      return Error("disk full");
    }
    return InMemoryDeliveryStore::AppendMessage(message);
  }

  mutable size_t annotation_reads = 0;
  std::string fail_append_once;
};

/**
 * One device: its keys, stores and receive pipeline. With `linked_to`: another device of that one's account. With
 * `probed`: its store is a ProbedStore.
 */
struct Device {
  Device(const std::string& tag, const uint8_t seed, const Device* linked_to = nullptr, const bool probed = false)
      : tag(tag) {
    keys = linked_to ? linked_to->keys : test::TestAccount::Make();
    if (linked_to) {
      keys.device = test::TestDevice::Make();
      keys.device_public = keys.device.signing_public;
    }
    account = keys.account_id;
    if (probed) {
      auto made = std::make_unique<ProbedStore>();
      probe = made.get();
      store = std::move(made);
    } else {
      store = std::make_unique<InMemoryDeliveryStore>();
    }
    EXPECT_TRUE(store->SetDek(TestDek(seed)) && store->ListThreads());
    roster = std::make_unique<InMemoryGroupRosterStore>(*store);
    psk = std::make_unique<InMemoryPeerSessions>(*store);
    EXPECT_TRUE(psk->SetDek(TestDek(seed)));
    pipeline = std::make_unique<RelayReceivePipeline>(
        *store,
        [this](const std::string& account_id) -> std::optional<AccountCard> {
          auto found = cards.find(account_id);
          return found == cards.end() ? std::nullopt : std::optional<AccountCard>(found->second);
        },
        *psk, test::ReceiveKeysOf(keys), *roster);
  }
  ~Device() {
    pipeline.reset();
    psk.reset();
    roster.reset();
    store.reset();
  }

  /** The cards this device keeps: `peer`'s (its one device). */
  void Know(const Device& peer) { cards[peer.account] = peer.keys.DeviceCard(); }

  /** Our own account's card, listing `devices` (all of this account), as the account signs it. */
  void KnowOwnDevices(const std::vector<const Device*>& devices) {
    AccountCard card{.keys = keys.Card(), .nickname = "test", .issued_at = 1};
    for (const Device* device : devices) {
      card.devices.push_back(device->keys.device.Entry());
    }
    cards[account] = SignAccountCard(card, [this](const ByteVector& bytes) { return keys.Sign(bytes); }).value();
  }

  EnvelopeSender Sender() const { return EnvelopeSender{"relay:" + tag, account, keys.device.peer_id}; }

  Thread DirectThreadTo(const Device& peer, const ThreadChannel channel) {
    DirectChatTarget target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.account;
    target.channel = channel;
    auto thread = store->FindOrCreateDirectThread(target, "", "peer");
    EXPECT_TRUE(thread);
    return *thread;
  }

  /** A direct send as MeshDeliveryOrchestrator runs it, minus the transports (the peer's devices not known yet). */
  Roe<RelayEnvelope> Send(const Thread& thread, const Device& peer, const std::string& text,
                          const SendRelayOptions& options = {}) {
    auto appended = AppendOutboundMessage(
        *store, NewOutboundMessage(thread.id, text, options, util::GenerateUuid(), util::NowUnixMs()));
    if (!appended) {
      return appended.error();
    }
    auto sealed = SealDirectPayload(*appended, thread, account, options, *psk,
                                    [&peer](const PeerSessionKey&) -> Roe<ByteVector> { return peer.keys.kem_public; });
    if (!sealed) {
      return sealed.error();
    }
    RelayEnvelope envelope = BuildDirectEnvelope(*appended, Sender(), thread.channel, "relay:" + peer.tag,
                                                 {AccountCopy(*appended, std::move(*sealed))}, NextRelayOrderKey());
    test::SignEnvelopeAs(keys, envelope);
    return envelope;
  }

  /** This device in `group_id` with `members` (owner first, then `admins` of them as admins) and the group's thread. */
  void JoinGroup(const std::string& group_id, const std::vector<const Device*>& members,
                 const std::vector<const Device*>& admins = {}) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = members.front()->account;
    metadata.title = "group";
    metadata.roster_epoch = 1;
    ASSERT_TRUE(roster->UpsertMetadata(metadata));
    std::vector<std::string> ids;
    for (const Device* m : members) {
      GroupRosterMember member;
      member.member_identity = m->account;
      member.contact_id = m->account;
      member.role = m == members.front() ? MemberRole::Owner
                    : std::find(admins.begin(), admins.end(), m) != admins.end() ? MemberRole::Admin
                                                                                 : MemberRole::Member;
      member.joined_at = 1;
      ASSERT_TRUE(roster->UpsertMember(group_id, member));
      ids.push_back(m->account);
      if (m != this) {
        Know(*m);
      }
    }
    auto thread = store->FindOrCreateGroupThread(group_id, "group", ids);
    ASSERT_TRUE(thread);
    group_thread = thread->id;
  }

  /** `message` (a text, or an edit: its whole ChatPayload) to group member `to`, as SendGroupMessage seals it. */
  RelayEnvelope SendGroup(const std::string& group_id, const Device& to, const ThreadMessage& message) {
    GroupMemberTarget target{to.account, GroupE2ePayloadCodec::PairTargetKey(to.account)};
    target.session_epoch = 1;
    target.sender_seq = ++group_seq[to.account];
    std::optional<std::vector<uint8_t>> rich;
    if (message.content_type != ChatContentType::Text) {
      rich = ChatPayloadCodec::EncodeToRow(message).value();
    }
    auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
        message.text, group_id, account, message.id, message.timestamp, {target}, *psk,
        [&to](const PeerSessionKey&) -> Roe<ByteVector> { return to.keys.kem_public; }, rich);
    EXPECT_TRUE(encrypted) << encrypted.error().message;
    EXPECT_TRUE(encrypted->copies.front());
    const RelayCopy copy{.session_epoch = target.session_epoch,
                         .sender_seq = target.sender_seq,
                         .key_init_b64 = encrypted->copies.front()->key_init_b64,
                         .payload_b64 = encrypted->copies.front()->payload_b64};
    RelayEnvelope envelope =
        BuildGroupMemberEnvelope(message, Sender(), group_id, to.account, to.account, {copy}, NextRelayOrderKey());
    test::SignEnvelopeAs(keys, envelope);
    return envelope;
  }

  /**
   * A control-lane envelope from this device carrying `notice`, sealed to account `to` (its account copy, a one-off
   * opener to its account KEM key) — as ControlLane::NotifySiblings sends our sent copies. `claimed_account` names the
   * sender (default: this account).
   */
  RelayEnvelope Lane(const Device& to, const ThreadMessage& notice, const std::string& claimed_account = {}) const {
    auto opened = AutoKeyEstablishment::EncapsulateForRecipient(to.keys.kem_public);
    EXPECT_TRUE(opened);
    ControlLaneSendParams params;
    params.sender_account_id = claimed_account.empty() ? account : claimed_account;
    params.sender_relay_id = "relay:" + tag;
    params.sender_device = keys.device.peer_id;
    params.peer_account_id = to.account;
    params.route_party = "relay:" + to.tag;
    params.copies = {
        ControlLaneCopy{.session_epoch = 1, .master_psk = opened->master_psk, .key_init_b64 = opened->key_init_b64}};
    params.order_key = NextRelayOrderKey();
    auto envelope = BuildControlLaneEnvelope(
        params, notice, [this](const std::vector<uint8_t>& bytes) { return keys.device.Sign(bytes); });
    EXPECT_TRUE(envelope) << envelope.error().message;
    return *envelope;
  }

  /** Our sent copy of `sent` (to `target`) from this device to the account's other devices (MirrorToSiblings). */
  RelayEnvelope SentCopyTo(const Device& sibling, const ThreadMessage& sent, const SentCopyTarget& target) const {
    return Lane(sibling, sent_copy::BuildNotice(sent, target).value());
  }

  RelayReceiveOutcome Receive(const RelayEnvelope& envelope) { return pipeline->ProcessEnvelope(envelope, account); }

  /** The row `id` of `thread_id`, if stored. */
  std::optional<ThreadMessage> Row(const std::string& thread_id, const std::string& id) {
    auto found = store->GetMessage(thread_id, id);
    return found && *found ? std::optional<ThreadMessage>(**found) : std::nullopt;
  }

  /** Rows of `thread_id` that hold `words` in their text or payload. */
  size_t RowsHolding(const std::string& thread_id, const std::string& words) {
    size_t n = 0;
    const std::vector<ThreadMessage> rows = store->GetMessages(thread_id).value(); // held: the Roe is a temporary
    for (const ThreadMessage& m : rows) {
      n += m.text.find(words) != std::string::npos || m.payload_json.find(words) != std::string::npos ? 1 : 0;
    }
    return n;
  }

  /** Edit rows aimed at `target_id` that still carry text. */
  size_t EditRowsWithText(const std::string& thread_id, const std::string& target_id) {
    size_t n = 0;
    const std::vector<ThreadMessage> rows = store->GetAnnotationsForTarget(thread_id, target_id).value();
    for (const ThreadMessage& m : rows) {
      n += IsEditAnnotation(m) && !EditTextOf(m).value_or("").empty() ? 1 : 0;
    }
    return n;
  }

  std::string tag;
  test::TestAccount keys;
  std::string account;
  std::map<std::string, AccountCard> cards;
  std::unique_ptr<InMemoryDeliveryStore> store;
  ProbedStore* probe = nullptr;
  std::unique_ptr<InMemoryGroupRosterStore> roster;
  std::unique_ptr<InMemoryPeerSessions> psk;
  std::unique_ptr<RelayReceivePipeline> pipeline;
  std::string group_thread;
  std::map<std::string, uint64_t> group_seq;
};

// ---- a group: only a message's own sender edits it; no member, admin or owner edits another's ----------------------

constexpr const char* kGroup = "group:hike";

class GroupEditPipelineTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    alice_ = std::make_unique<Device>("alice", 0x10); // owner
    carol_ = std::make_unique<Device>("carol", 0x20); // admin
    dave_ = std::make_unique<Device>("dave", 0x30);   // writes the message
    erin_ = std::make_unique<Device>("erin", 0x40);   // a member
    bob_ = std::make_unique<Device>("bob", 0x50);     // this device, a member
    const std::vector<const Device*> members{alice_.get(), carol_.get(), dave_.get(), erin_.get(), bob_.get()};
    bob_->JoinGroup(kGroup, members, {carol_.get()});
    erin_->JoinGroup(kGroup, members, {carol_.get()});
    sent_ = util::NowUnixMs();
  }

  /** Dave's message `id` reaches `to`. */
  void DaveSays(Device& to, const std::string& id = "dave-1") {
    const RelayReceiveOutcome got = to.Receive(dave_->SendGroup(kGroup, to, Text(id, "see you at 8", sent_)));
    ASSERT_TRUE(got.persisted) << got.receive_failure_detail;
  }

  std::unique_ptr<Device> alice_, carol_, dave_, erin_, bob_;
  int64_t sent_ = 0;
};

TEST_F(GroupEditPipelineTest, AMembersEditOfAnotherMembersMessageChangesNothing) {
  DaveSays(*bob_);
  const RelayReceiveOutcome got =
      bob_->Receive(erin_->SendGroup(kGroup, *bob_, Edit("erin-edit", "dave-1", "forged words", sent_ + kMin)));
  EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
  const ThreadMessage dave = bob_->Row(bob_->group_thread, "dave-1").value();
  EXPECT_EQ(dave.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(dave));
  EXPECT_EQ(bob_->RowsHolding(bob_->group_thread, "forged words"), 0u) << "refused, and its words not kept";
}

TEST_F(GroupEditPipelineTest, NeitherTheAdminNorTheOwnerEditsAnotherMembersMessage) {
  DaveSays(*bob_);
  EXPECT_TRUE(
      bob_->Receive(carol_->SendGroup(kGroup, *bob_, Edit("carol-edit", "dave-1", "admin words", sent_ + kMin)))
          .persisted)
      << "taken in (and refused as an edit)";
  EXPECT_TRUE(
      bob_->Receive(alice_->SendGroup(kGroup, *bob_, Edit("alice-edit", "dave-1", "owner words", sent_ + 2 * kMin)))
          .persisted);
  const ThreadMessage dave = bob_->Row(bob_->group_thread, "dave-1").value();
  EXPECT_EQ(dave.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(dave));
  EXPECT_EQ(bob_->RowsHolding(bob_->group_thread, "admin words"), 0u);
  EXPECT_EQ(bob_->RowsHolding(bob_->group_thread, "owner words"), 0u);
}

TEST_F(GroupEditPipelineTest, TheAuthorsEditAppliesForEveryMember) {
  for (Device* member : {bob_.get(), erin_.get()}) {
    DaveSays(*member);
    const RelayReceiveOutcome got =
        member->Receive(dave_->SendGroup(kGroup, *member, Edit("dave-edit", "dave-1", "see you at 9", sent_ + kMin)));
    EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
    EXPECT_TRUE(got.suppress_inbox_chrome) << "no unread count, preview or notice";
    const ThreadMessage dave = member->Row(member->group_thread, "dave-1").value();
    EXPECT_EQ(dave.text, "see you at 9") << member->tag;
    ASSERT_TRUE(EditMarkOf(dave));
    EXPECT_EQ(EditMarkOf(dave).value_or(EditMark{}).edited_by, "dave-edit");
  }
}

// The edit overtakes its message, and it is not the author's: when the message comes it is judged, and refused.
TEST_F(GroupEditPipelineTest, AnEditFromAnotherMemberThatArrivesFirstIsNotAppliedWhenTheMessageComes) {
  ASSERT_TRUE(
      bob_->Receive(erin_->SendGroup(kGroup, *bob_, Edit("erin-edit", "dave-1", "forged words", sent_ + kMin)))
          .persisted);
  DaveSays(*bob_);
  const ThreadMessage dave = bob_->Row(bob_->group_thread, "dave-1").value();
  EXPECT_EQ(dave.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(dave));
  EXPECT_EQ(bob_->RowsHolding(bob_->group_thread, "forged words"), 0u);
}

// ---- our other devices: our edit made on one reaches the others as a sent copy ---------------------------------------

class SentCopyEditPipelineTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    phone_ = std::make_unique<Device>("me", 0x10);                    // this device
    mac_ = std::make_unique<Device>("me-mac", 0x20, phone_.get());    // our other device
    bob_ = std::make_unique<Device>("bob", 0x30);                     // the peer
    mallory_ = std::make_unique<Device>("mallory", 0x40);             // someone else
    phone_->KnowOwnDevices({phone_.get(), mac_.get()});
    phone_->Know(*mallory_);
    chat_ = phone_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
    sent_ = util::NowUnixMs();
  }

  /** Our message `id` in the public chat with Bob, out on the wire. */
  void OurMessage(const Thread& thread, const std::string& id) {
    ThreadMessage mine = Text(id, "see you at 8", sent_);
    mine.thread_id = thread.id;
    mine.sender_contact_id = kLocalSelfContactId;
    mine.delivery = MessageDelivery::Relayed;
    mine.relay_visible = true;
    ASSERT_TRUE(phone_->store->AppendMessage(mine));
  }

  SentCopyTarget ToBob() const { return SentCopyTarget{.peer_account = bob_->account}; }

  std::unique_ptr<Device> phone_, mac_, bob_, mallory_;
  Thread chat_;
  int64_t sent_ = 0;
};

TEST_F(SentCopyEditPipelineTest, OurEditFromTheOtherDeviceApplies) {
  OurMessage(chat_, "mine-1");
  const RelayReceiveOutcome got =
      phone_->Receive(mac_->SentCopyTo(*phone_, Edit("mac-edit", "mine-1", "see you at 9", sent_ + kMin), ToBob()));
  EXPECT_TRUE(got.persisted);
  EXPECT_TRUE(got.sibling_copy);
  EXPECT_TRUE(got.suppress_inbox_chrome);
  const ThreadMessage mine = phone_->Row(chat_.id, "mine-1").value();
  EXPECT_EQ(mine.text, "see you at 9");
  ASSERT_TRUE(EditMarkOf(mine));
  EXPECT_EQ(EditMarkOf(mine).value_or(EditMark{}).edited_by, "mac-edit");
}

TEST_F(SentCopyEditPipelineTest, OurEditThatArrivesBeforeItsMessageAppliesWhenTheMessageComes) {
  ASSERT_TRUE(
      phone_->Receive(mac_->SentCopyTo(*phone_, Edit("mac-edit", "mine-2", "see you at 9", sent_ + kMin), ToBob()))
          .persisted);
  ASSERT_TRUE(phone_->Receive(mac_->SentCopyTo(*phone_, Text("mine-2", "see you at 8", sent_), ToBob())).persisted);
  const ThreadMessage mine = phone_->Row(chat_.id, "mine-2").value();
  EXPECT_EQ(mine.sender_contact_id, kLocalSelfContactId);
  EXPECT_EQ(mine.text, "see you at 9");
  EXPECT_EQ(phone_->EditRowsWithText(chat_.id, "mine-2"), 1u);
}

// A "sent copy" from anyone but our own account is not one: as itself, the lane drops it (no family takes it); claiming
// our account, its signature does not verify with our card. Our message keeps its words either way.
TEST_F(SentCopyEditPipelineTest, ASentCopyFromAnotherAccountRewritesNothing) {
  OurMessage(chat_, "mine-1");
  const ThreadMessage forged_notice =
      sent_copy::BuildNotice(Edit("forged-edit", "mine-1", "forged words", sent_ + kMin), ToBob()).value();

  const RelayReceiveOutcome as_herself = phone_->Receive(mallory_->Lane(*phone_, forged_notice));
  EXPECT_FALSE(as_herself.persisted);
  EXPECT_FALSE(as_herself.sibling_copy);
  EXPECT_TRUE(as_herself.sender_verified) << "she is who she says: read, then dropped (no lane family takes it)";
  EXPECT_EQ(as_herself.decision, IngestDecision::SilentDiscard);

  const RelayReceiveOutcome as_us = phone_->Receive(mallory_->Lane(*phone_, forged_notice, phone_->account));
  EXPECT_FALSE(as_us.persisted);
  EXPECT_FALSE(as_us.sibling_copy);
  EXPECT_FALSE(as_us.sender_verified) << "her device is not on our card";

  const ThreadMessage mine = phone_->Row(chat_.id, "mine-1").value();
  EXPECT_EQ(mine.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(mine));
  EXPECT_EQ(phone_->RowsHolding(chat_.id, "forged words"), 0u);
  EXPECT_FALSE(phone_->Row(chat_.id, "forged-edit"));
}

// A private chat stays on its device: a sent copy is only ever filed in the public chat with that peer, so our message
// in the private chat is not touched even by a copy aimed at its id. (Our sends from a private chat make no copy at all:
// MeshDeliveryOrchestrator::SendUserMessage mirrors public chats only — that rule is outside the pipeline.)
TEST_F(SentCopyEditPipelineTest, ACopyNeverReachesAPrivateChat) {
  const Thread secure = phone_->DirectThreadTo(*bob_, ThreadChannel::E2e);
  ASSERT_NE(secure.id, chat_.id);
  OurMessage(secure, "secret-1");
  const RelayReceiveOutcome got =
      phone_->Receive(mac_->SentCopyTo(*phone_, Edit("mac-edit", "secret-1", "copied words", sent_ + kMin), ToBob()));
  EXPECT_TRUE(got.persisted);
  EXPECT_EQ(got.thread_id, chat_.id) << "filed in the public chat with Bob";
  const ThreadMessage mine = phone_->Row(secure.id, "secret-1").value();
  EXPECT_EQ(mine.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(mine));
  EXPECT_EQ(phone_->RowsHolding(secure.id, "copied words"), 0u);
}

// ---- the public channel (e2e_public, AutoKey) -------------------------------------------------------------------------

class PublicEditPipelineTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    alice_ = std::make_unique<Device>("alice", 0x10);
    bob_ = std::make_unique<Device>("bob", 0x40);
    alice_->Know(*bob_);
    bob_->Know(*alice_);
    alice_chat_ = alice_->DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  }

  /** Alice's message, received by Bob; returns it as Alice holds it (out on the wire). */
  ThreadMessage AliceSays(const std::string& text) {
    auto sent = alice_->Send(alice_chat_, *bob_, text);
    EXPECT_TRUE(sent) << sent.error().message;
    EXPECT_TRUE(bob_->Receive(*sent).persisted);
    ThreadMessage mine = alice_->Row(alice_chat_.id, sent->message_id).value();
    mine.delivery = MessageDelivery::Relayed;
    EXPECT_TRUE(alice_->store->UpdateMessage(mine));
    return mine;
  }

  /** Alice's edit of `target` as the send puts it on the wire, stamped at `at`. */
  Roe<RelayEnvelope> AliceEdits(const ThreadMessage& target, const std::string& edit_id, const std::string& text,
                                const int64_t at) {
    return alice_->Send(alice_chat_, *bob_, text, EditSendOptions(target.id, text, edit_id, at));
  }

  std::string BobsChat() { return bob_->DirectThreadTo(*alice_, ThreadChannel::E2ePublic).id; }

  std::unique_ptr<Device> alice_, bob_;
  Thread alice_chat_;
};

TEST_F(PublicEditPipelineTest, ThePeersEditApplies) {
  const ThreadMessage said = AliceSays("see you at 8");
  auto edit = AliceEdits(said, "edit-1", "see you at 9", said.timestamp + kMin);
  ASSERT_TRUE(edit) << edit.error().message;
  const RelayReceiveOutcome got = bob_->Receive(*edit);
  EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
  EXPECT_TRUE(got.suppress_inbox_chrome);
  const ThreadMessage bobs = bob_->Row(BobsChat(), said.id).value();
  EXPECT_EQ(bobs.text, "see you at 9");
  EXPECT_EQ(EditMarkOf(bobs).value_or(EditMark{}).edited_by, "edit-1");
}

// Bob aims an edit at Alice's message on the public channel: Alice's copy keeps its words.
TEST_F(PublicEditPipelineTest, AnEditFromSomeoneButTheAuthorIsRefused) {
  const ThreadMessage said = AliceSays("see you at 8");
  const Thread bob_chat = bob_->DirectThreadTo(*alice_, ThreadChannel::E2ePublic);
  auto forged = bob_->Send(bob_chat, *alice_, "forged words",
                           EditSendOptions(said.id, "forged words", "bob-edit", said.timestamp + kMin));
  ASSERT_TRUE(forged) << forged.error().message;
  const RelayReceiveOutcome got = alice_->Receive(*forged);
  EXPECT_TRUE(got.persisted) << got.receive_failure_detail;
  const ThreadMessage mine = alice_->Row(alice_chat_.id, said.id).value();
  EXPECT_EQ(mine.text, "see you at 8");
  EXPECT_FALSE(EditMarkOf(mine));
  EXPECT_EQ(alice_->RowsHolding(alice_chat_.id, "forged words"), 0u);
}

// ---- a resent edit (the red mark tapped): same words and send time, a new id ------------------------------------------

// The first send of the edit is stored and fails (its envelope may still get through later); the resend goes out as a
// new id. Here: the message carries the new id, the unsent row is blanked and taken away, and the queued send of it is
// cancelled. The peer may get both: the old id, then the new — the same words and time — and ends with those words and
// one edit row holding them, whichever id sorts higher.
TEST_F(PublicEditPipelineTest, AResentEditReplacesTheUnsentOneAndThePeerEndsWithOneCopyOfTheWords) {
  // The new id sorting below the old one, then above it.
  for (const auto& [old_id, new_id] : std::vector<std::pair<std::string, std::string>>{{"edit-1a", "edit-0a"},
                                                                                        {"edit-1b", "edit-2b"}}) {
    SCOPED_TRACE(new_id);
    const ThreadMessage said = AliceSays("see you at 8");
    const int64_t tapped_at = said.timestamp + kMin;

    std::optional<RelayEnvelope> lost;
    auto first = EditOwnMessage(*alice_->store, said, ThreadKind::Direct, "see you at 9", tapped_at, old_id,
                                [&](const std::string& edit_id, const std::string& text, const int64_t sent_at) {
                                  auto envelope = AliceEdits(said, edit_id, text, sent_at);
                                  EXPECT_TRUE(envelope);
                                  lost = *envelope;
                                  ThreadMessage row = alice_->Row(alice_chat_.id, edit_id).value();
                                  row.delivery = MessageDelivery::Failed; // as FailOutbound leaves it
                                  EXPECT_TRUE(alice_->store->UpdateMessage(row));
                                  return Roe<ThreadMessage>(Error("relay unreachable"));
                                },
                                {});
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_TRUE(first->stored_unsent);
    EXPECT_EQ(EditMarkOf(alice_->Row(alice_chat_.id, said.id).value()).value_or(EditMark{}).edited_by, old_id);

    std::optional<RelayEnvelope> resent_envelope;
    std::vector<std::string> cancelled;
    auto resent = ResendOwnEdit(
        *alice_->store, alice_chat_.id, said.id, new_id,
        [&](const std::string& edit_id, const std::string& text, const int64_t sent_at) {
          auto envelope = AliceEdits(said, edit_id, text, sent_at);
          EXPECT_TRUE(envelope);
          resent_envelope = *envelope;
          return Roe<ThreadMessage>(alice_->Row(alice_chat_.id, edit_id).value());
        },
        [&](const std::string& unsent_id) { cancelled.push_back(unsent_id); });
    ASSERT_TRUE(resent) << resent.error().message;
    EXPECT_EQ(resent->edit_row.id, new_id);
    EXPECT_TRUE(resent->unsent_removed);
    EXPECT_EQ(cancelled, std::vector<std::string>{old_id});
    const ThreadMessage mine = alice_->Row(alice_chat_.id, said.id).value();
    EXPECT_EQ(mine.text, "see you at 9");
    EXPECT_EQ(EditMarkOf(mine).value_or(EditMark{}).edited_by, new_id);
    EXPECT_FALSE(alice_->Row(alice_chat_.id, old_id)) << "the unsent edit row is gone";
    EXPECT_EQ(alice_->EditRowsWithText(alice_chat_.id, said.id), 1u);

    ASSERT_TRUE(lost && resent_envelope);
    EXPECT_EQ(resent_envelope->timestamp, lost->timestamp) << "the same send time";
    EXPECT_TRUE(bob_->Receive(*lost).persisted);
    EXPECT_TRUE(bob_->Receive(*resent_envelope).persisted);
    const ThreadMessage bobs = bob_->Row(BobsChat(), said.id).value();
    EXPECT_EQ(bobs.text, "see you at 9");
    EXPECT_EQ(bob_->EditRowsWithText(BobsChat(), said.id), 1u);
  }
}

// An edit that already went out is not sent again.
TEST_F(PublicEditPipelineTest, AnEditThatWentOutIsNotSentAgain) {
  const ThreadMessage said = AliceSays("see you at 8");
  auto edited =
      EditOwnMessage(*alice_->store, said, ThreadKind::Direct, "see you at 9", said.timestamp + kMin, "edit-1",
                     [&](const std::string& edit_id, const std::string& text, const int64_t at) {
                       EXPECT_TRUE(AliceEdits(said, edit_id, text, at));
                       return Roe<ThreadMessage>(alice_->Row(alice_chat_.id, edit_id).value());
                     },
                     {});
  ASSERT_TRUE(edited) << edited.error().message;
  ThreadMessage row = alice_->Row(alice_chat_.id, "edit-1").value();
  row.delivery = MessageDelivery::Relayed; // handed to the relay
  ASSERT_TRUE(alice_->store->UpdateMessage(row));

  bool sent = false;
  bool cancelled = false;
  auto resent = ResendOwnEdit(
      *alice_->store, alice_chat_.id, said.id, "edit-2",
      [&](const std::string&, const std::string&, int64_t) {
        sent = true;
        return Roe<ThreadMessage>(Error("not expected"));
      },
      [&](const std::string&) { cancelled = true; });
  ASSERT_FALSE(resent);
  EXPECT_EQ(resent.error().message, "Nothing to send again");
  EXPECT_FALSE(sent);
  EXPECT_FALSE(cancelled);
  EXPECT_EQ(EditMarkOf(alice_->Row(alice_chat_.id, said.id).value()).value_or(EditMark{}).edited_by, "edit-1");
  EXPECT_TRUE(alice_->Row(alice_chat_.id, "edit-1"));
}

// ---- an edit of ours that is voided before it goes out is never sent ------------------------------------------------

// Alice is offline: what she sends waits in the queue (FakeOutbox) until she is back.
class UnsentEditPipelineTest : public PublicEditPipelineTest {
protected:
  /** Alice edits `target` to `text` at `at` as ConversationsHub::EditMessage does: queued, applied here. */
  void AliceEditsOffline(const ThreadMessage& target, const std::string& edit_id, const std::string& text,
                         const int64_t at) {
    auto edited = EditOwnMessage(
        *alice_->store, target, ThreadKind::Direct, text, at, edit_id,
        [&](const std::string& id, const std::string& words, const int64_t sent_at) {
          auto envelope = AliceEdits(target, id, words, sent_at);
          EXPECT_TRUE(envelope) << envelope.error().message;
          outbox_.Send(id, *envelope);
          return Roe<ThreadMessage>(alice_->Row(alice_chat_.id, id).value());
        },
        [&](const std::string& id) { outbox_.Cancel(id); });
    ASSERT_TRUE(edited) << edited.error().message;
  }

  /**
   * Alice deletes `target` for everyone as ConversationsHub::DeleteMessageForEveryone does: its own queued send
   * cancelled, the retract queued, then deleted here (InboxController::DeleteMessageForMe: the row, its edit rows).
   */
  void AliceDeletesForEveryone(const ThreadMessage& target, const std::string& retract_id) {
    outbox_.Cancel(target.id);
    SendRelayOptions options;
    options.content_type = ChatContentType::Annotation;
    options.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, target.id, "");
    options.update_preview = false;
    options.message_id = retract_id;
    auto retract = alice_->Send(alice_chat_, *bob_, "", options);
    ASSERT_TRUE(retract) << retract.error().message;
    outbox_.Send(retract_id, *retract);
    ASSERT_TRUE(alice_->store->DeleteMessageContent(alice_chat_.id, target.id));
    (void)BlankEditRowsFor(*alice_->store, alice_chat_.id, target.id, {},
                           [&](const std::string& id) { outbox_.Cancel(id); });
  }

  /** Bob takes in everything that went out. */
  void BobReceivesWhatWentOut() {
    for (const RelayEnvelope& envelope : outbox_.sent) {
      EXPECT_TRUE(bob_->Receive(envelope).persisted) << envelope.message_id;
    }
  }

  FakeOutbox outbox_;
};

TEST_F(UnsentEditPipelineTest, AnEditQueuedOfflineThenDeletedForEveryoneNeverGoesOut) {
  const ThreadMessage said = AliceSays("see you at 8");
  AliceEditsOffline(said, "edit-1", "meet me at the secret place", said.timestamp + kMin);
  ASSERT_EQ(outbox_.queued.size(), 1u);
  AliceDeletesForEveryone(alice_->Row(alice_chat_.id, said.id).value(), "retract-1");

  outbox_.Retry();
  EXPECT_EQ(outbox_.SentIds(), std::vector<std::string>{"retract-1"}) << "the delete only";
  BobReceivesWhatWentOut();
  EXPECT_EQ(bob_->Row(BobsChat(), said.id).value().content_type, ChatContentType::Deleted);
  EXPECT_EQ(bob_->RowsHolding(BobsChat(), "secret place"), 0u);
  EXPECT_EQ(alice_->RowsHolding(alice_chat_.id, "secret place"), 0u);
}

TEST_F(UnsentEditPipelineTest, TwoEditsQueuedOfflineSendOnlyTheSecond) {
  const ThreadMessage said = AliceSays("see you at 8");
  AliceEditsOffline(said, "edit-1", "see you at 9", said.timestamp + kMin);
  AliceEditsOffline(alice_->Row(alice_chat_.id, said.id).value(), "edit-2", "see you at 10", said.timestamp + 2 * kMin);

  outbox_.Retry();
  EXPECT_EQ(outbox_.SentIds(), std::vector<std::string>{"edit-2"});
  BobReceivesWhatWentOut();
  const ThreadMessage bobs = bob_->Row(BobsChat(), said.id).value();
  EXPECT_EQ(bobs.text, "see you at 10");
  EXPECT_EQ(bob_->RowsHolding(BobsChat(), "at 9"), 0u);
  EXPECT_EQ(alice_->RowsHolding(alice_chat_.id, "at 9"), 0u);
}

// Sent before the delete (handed to the relay): as before, the delete follows it and nothing is sent twice.
TEST_F(UnsentEditPipelineTest, AnEditThatWentOutBeforeTheDeleteIsFollowedByTheDeleteOnly) {
  outbox_.online = true;
  const ThreadMessage said = AliceSays("see you at 8");
  AliceEditsOffline(said, "edit-1", "see you at 9", said.timestamp + kMin);
  ThreadMessage edit_row = alice_->Row(alice_chat_.id, "edit-1").value();
  edit_row.delivery = MessageDelivery::Relayed;
  ASSERT_TRUE(alice_->store->UpdateMessage(edit_row));
  AliceDeletesForEveryone(alice_->Row(alice_chat_.id, said.id).value(), "retract-1");

  outbox_.Retry();
  EXPECT_EQ(outbox_.SentIds(), (std::vector<std::string>{"edit-1", "retract-1"}));
  BobReceivesWhatWentOut();
  EXPECT_EQ(bob_->Row(BobsChat(), said.id).value().content_type, ChatContentType::Deleted);
  EXPECT_EQ(bob_->RowsHolding(BobsChat(), "at 9"), 0u);
}

// Our edit on this device waits in its queue; another of our devices deletes the message, or edits it again, and its
// sent copy arrives first: the queued edit is not sent.
class SentCopyUnsentEditTest : public SentCopyEditPipelineTest {
protected:
  void SetUp() override {
    SentCopyEditPipelineTest::SetUp();
    phone_->pipeline->SetCancelPendingSend([this](const std::string& id) { outbox_.Cancel(id); });
    OurMessage(chat_, "mine-1");
    const ThreadMessage mine = phone_->Row(chat_.id, "mine-1").value();
    auto edited = EditOwnMessage(
        *phone_->store, mine, ThreadKind::Direct, "the secret words", sent_ + kMin, "phone-edit",
        [&](const std::string& id, const std::string& words, const int64_t at) {
          auto envelope = phone_->Send(chat_, *bob_, words, EditSendOptions(mine.id, words, id, at));
          EXPECT_TRUE(envelope) << envelope.error().message;
          outbox_.Send(id, *envelope);
          return Roe<ThreadMessage>(phone_->Row(chat_.id, id).value());
        },
        [&](const std::string& id) { outbox_.Cancel(id); });
    ASSERT_TRUE(edited) << edited.error().message;
    ASSERT_EQ(outbox_.queued.size(), 1u);
  }

  FakeOutbox outbox_;
};

TEST_F(SentCopyUnsentEditTest, OurDeleteFromTheOtherDeviceCancelsIt) {
  ASSERT_TRUE(
      phone_->Receive(mac_->SentCopyTo(*phone_, Retract("mac-delete", "mine-1", sent_ + 2 * kMin), ToBob())).persisted);
  EXPECT_EQ(phone_->Row(chat_.id, "mine-1").value().content_type, ChatContentType::Deleted);
  outbox_.Retry();
  EXPECT_TRUE(outbox_.sent.empty()) << "the deleted message's edit is not sent";
  EXPECT_EQ(phone_->RowsHolding(chat_.id, "secret words"), 0u);
}

TEST_F(SentCopyUnsentEditTest, ANewerEditFromTheOtherDeviceCancelsIt) {
  ASSERT_TRUE(
      phone_->Receive(mac_->SentCopyTo(*phone_, Edit("mac-edit", "mine-1", "see you at 9", sent_ + 2 * kMin), ToBob()))
          .persisted);
  EXPECT_EQ(phone_->Row(chat_.id, "mine-1").value().text, "see you at 9");
  outbox_.Retry();
  EXPECT_TRUE(outbox_.sent.empty()) << "the replaced edit is not sent (the other device sends its own)";
  EXPECT_EQ(phone_->RowsHolding(chat_.id, "secret words"), 0u);
}

// ---- one edit row keeps its words, and an applied edit costs a few row reads, not a read of all of them -------------

class EditCostTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    dave_ = std::make_unique<Device>("dave", 0x30); // writes the message, and edits it
    erin_ = std::make_unique<Device>("erin", 0x40); // another member
    bob_ = std::make_unique<Device>("bob", 0x50, nullptr, /*probed=*/true);
    bob_->JoinGroup(kGroup, {dave_.get(), erin_.get(), bob_.get()});
    sent_ = util::NowUnixMs();
  }

  RelayEnvelope DaveEdits(const std::string& id, const std::string& text, const int64_t at) {
    return dave_->SendGroup(kGroup, *bob_, Edit(id, "dave-1", text, at));
  }

  std::unique_ptr<Device> dave_, erin_, bob_;
  int64_t sent_ = 0;
};

// 200 edits of one message in a random order (the message itself somewhere among them), mixing the author's edits in
// and out of the 16 minutes (some at the same time), another member's, and envelopes delivered twice. At the end
// exactly one edit row has words: the one the message names (edited_by), the newest of the author's edits in time,
// whose words the message shows.
TEST_F(EditCostTest, TwoHundredEditsInAnyOrderLeaveOneEditRowWithWordsTheOneApplied) {
  for (const unsigned seed : {1u, 2u, 3u}) {
    SCOPED_TRACE(seed);
    SetUp(); // fresh devices per round
    std::mt19937 random(seed);
    struct Sent {
      RelayEnvelope envelope;
      std::string id;
      std::string text;
      int64_t at = 0;
      bool by_author = false;
    };
    std::vector<Sent> edits;
    for (int i = 0; i < 180; ++i) {
      const std::string id = "edit-" + std::to_string(1000 + i);
      const std::string text = "words " + std::to_string(i);
      const int kind = static_cast<int>(random() % 6); // 0..3 author in time, 4 author late, 5 another member
      const int64_t at = kind <= 3   ? sent_ + 1 + static_cast<int64_t>(random() % (16 * 60)) * 1000 // ties happen
                         : kind == 4 ? sent_ + 17 * kMin + static_cast<int64_t>(random() % (10 * 60)) * 1000
                                     : sent_ + static_cast<int64_t>(random() % (10 * 60)) * 1000;
      Device& from = kind == 5 ? *erin_ : *dave_;
      edits.push_back(Sent{from.SendGroup(kGroup, *bob_, Edit(id, "dave-1", text, at)), id, text, at, kind != 5});
    }
    std::vector<const RelayEnvelope*> deliveries;
    for (const Sent& edit : edits) {
      deliveries.push_back(&edit.envelope);
    }
    for (int i = 0; i < 20; ++i) {
      deliveries.push_back(&edits[random() % edits.size()].envelope); // delivered twice
    }
    const RelayEnvelope message = dave_->SendGroup(kGroup, *bob_, Text("dave-1", "see you at 8", sent_));
    deliveries.push_back(&message);
    std::shuffle(deliveries.begin(), deliveries.end(), random);

    std::set<std::string> stored;
    for (const RelayEnvelope* envelope : deliveries) {
      if (bob_->Receive(*envelope).persisted) {
        stored.insert(envelope->message_id);
      }
    }
    ASSERT_TRUE(stored.count("dave-1")) << "the message is stored";
    const Sent* newest = nullptr;
    for (const Sent& edit : edits) {
      if (stored.count(edit.id) && edit.by_author && edit.at - sent_ <= kEditWindowMs + kEditArrivalSlackMs &&
          (!newest || edit.at > newest->at || (edit.at == newest->at && edit.id > newest->id))) {
        newest = &edit;
      }
    }
    ASSERT_NE(newest, nullptr);

    const ThreadMessage dave = bob_->Row(bob_->group_thread, "dave-1").value();
    const std::optional<EditMark> mark = EditMarkOf(dave);
    ASSERT_TRUE(mark);
    EXPECT_EQ(mark->edited_by, newest->id);
    EXPECT_EQ(dave.text, newest->text);
    std::vector<std::string> with_words;
    const std::vector<ThreadMessage> rows = bob_->store->GetAnnotationsForTarget(bob_->group_thread, "dave-1").value();
    for (const ThreadMessage& row : rows) {
      if (IsEditAnnotation(row) && !EditTextOf(row).value_or("").empty()) {
        with_words.push_back(row.id);
      }
    }
    EXPECT_EQ(with_words, std::vector<std::string>{mark->edited_by});
  }
}

// The message is here: each new edit of it that applies, or that is refused, reads no list of its annotation rows.
TEST_F(EditCostTest, AnEditOfAMessageThatIsHereReadsNotAllItsEditRows) {
  ASSERT_TRUE(bob_->Receive(dave_->SendGroup(kGroup, *bob_, Text("dave-1", "see you at 8", sent_))).persisted);
  for (int i = 1; i <= 20; ++i) {
    const size_t before = bob_->probe->annotation_reads;
    ASSERT_TRUE(
        bob_->Receive(DaveEdits("edit-" + std::to_string(100 + i), "words " + std::to_string(i), sent_ + i * 1000))
            .persisted);
    EXPECT_EQ(bob_->probe->annotation_reads, before) << "applied edit " << i;
  }
  const size_t before = bob_->probe->annotation_reads;
  ASSERT_TRUE(bob_->Receive(DaveEdits("edit-old", "old words", sent_ + 500)).persisted); // older than the applied one
  ASSERT_TRUE(
      bob_->Receive(erin_->SendGroup(kGroup, *bob_, Edit("erin-edit", "dave-1", "forged", sent_ + kMin))).persisted);
  EXPECT_EQ(bob_->probe->annotation_reads, before) << "refused edits";
  EXPECT_EQ(bob_->Row(bob_->group_thread, "dave-1").value().text, "words 20");
  EXPECT_EQ(bob_->EditRowsWithText(bob_->group_thread, "dave-1"), 1u);
}

// Our own edits, one after another, the same.
TEST_F(EditCostTest, OurOwnEditsReadNotAllTheEditRows) {
  Device alice("alice", 0x10, nullptr, /*probed=*/true);
  const Thread chat = alice.DirectThreadTo(*bob_, ThreadChannel::E2ePublic);
  ThreadMessage mine = Text("mine-1", "see you at 8", sent_);
  mine.thread_id = chat.id;
  mine.sender_contact_id = kLocalSelfContactId;
  mine.delivery = MessageDelivery::Relayed;
  mine.relay_visible = true;
  ASSERT_TRUE(alice.store->AppendMessage(mine));
  for (int i = 1; i <= 10; ++i) {
    const ThreadMessage target = alice.Row(chat.id, "mine-1").value();
    const size_t before = alice.probe->annotation_reads;
    auto edited = EditOwnMessage(*alice.store, target, ThreadKind::Direct, "words " + std::to_string(i),
                                 sent_ + i * 1000, "own-" + std::to_string(i),
                                 [&](const std::string& id, const std::string& text, const int64_t at) {
                                   EXPECT_TRUE(alice.Send(chat, *bob_, text, EditSendOptions(target.id, text, id, at)));
                                   return Roe<ThreadMessage>(alice.Row(chat.id, id).value());
                                 },
                                 {});
    ASSERT_TRUE(edited) << edited.error().message;
    EXPECT_EQ(alice.probe->annotation_reads, before) << "edit " << i;
  }
  EXPECT_EQ(alice.EditRowsWithText(chat.id, "mine-1"), 1u);
  EXPECT_EQ(EditMarkOf(alice.Row(chat.id, "mine-1").value()).value_or(EditMark{}).edited_by, "own-10");
}

} // namespace
} // namespace pbr
