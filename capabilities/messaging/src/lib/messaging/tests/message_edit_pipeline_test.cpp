// Editing a sent message (DEV/PLAN-message-edit.md) through the receive pipeline on the paths the private-chat cases in
// messaging_cross_cutting_test.cpp do not take: a group, our own other devices (sent_copy on the control lane), the
// public channel, and a resent edit. Every envelope is built and sealed by the product's own send code (OutboundMessage,
// GroupE2ePayloadCodec, BuildControlLaneEnvelope, sent_copy) and read by a real RelayReceivePipeline.
#include "lib/delivery/AutoKeyEstablishment.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/delivery/OutboundMessage.h"
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
#include <memory>
#include <optional>
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

/** One device: its keys, stores and receive pipeline. With `linked_to`: another device of that one's account. */
struct Device {
  Device(const std::string& tag, const uint8_t seed, const Device* linked_to = nullptr) : tag(tag) {
    keys = linked_to ? linked_to->keys : test::TestAccount::Make();
    if (linked_to) {
      keys.device = test::TestDevice::Make();
      keys.device_public = keys.device.signing_public;
    }
    account = keys.account_id;
    store = std::make_unique<InMemoryDeliveryStore>();
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
                                });
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
  auto edited = EditOwnMessage(*alice_->store, said, ThreadKind::Direct, "see you at 9", said.timestamp + kMin,
                               "edit-1", [&](const std::string& edit_id, const std::string& text, const int64_t at) {
                                 EXPECT_TRUE(AliceEdits(said, edit_id, text, at));
                                 return Roe<ThreadMessage>(alice_->Row(alice_chat_.id, edit_id).value());
                               });
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

} // namespace
} // namespace pbr
