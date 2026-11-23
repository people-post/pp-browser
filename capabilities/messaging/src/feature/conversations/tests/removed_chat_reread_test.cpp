// A relay mailbox read again from its start (515eecb3e: a new registration id, or a cursor file from an older app) holds
// the records of messages the person removed here. What was removed stays removed: a message deleted here (its row a
// tombstone), a cleared chat, a deleted chat — no row, no chat made again, no unread count, no notice — while a new
// message still arrives. Alice's app (stores, inbox, the real MeshDeliveryOrchestrator) polls a relay that hands the
// same records over again; Carol writes to her as a peer does.
#include "feature/conversations/InboxController.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "feature/conversations/PeerDisplaySources.h"

#include "common/Utilities.h"
#include "common/chat/RelayStreamKey.h"
#include "common/runtime/AppRuntime.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/AutoKeyEstablishment.h"
#include "lib/delivery/ControlLaneEnvelope.h"
#include "lib/delivery/OutboundMessage.h"
#include "lib/delivery/ReactionTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/SentCopy.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/PeerCards.h"
#include "lib/people/PeerKemKeyStore.h"
#include "lib/people/PeerSigningKeyStore.h"
#include "lib/people/testing/InMemoryPeerCards.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector TestDek(const uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

/** A relay mailbox: each poll hands over what was put in since (a re-read is the same records put in again). */
class MailboxRelay final : public IRelayClient {
public:
  void Put(RelayEnvelope envelope) {
    std::lock_guard lock(mu_);
    inbox_.push_back(std::move(envelope));
  }
  Roe<void> Send(const RelayEnvelope&) override { return {}; }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override {
    std::lock_guard lock(mu_);
    RelayPollResult result;
    result.messages.swap(inbox_);
    result.next_cursor = "c" + std::to_string(++polls_);
    return result;
  }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override {
    std::lock_guard lock(mu_);
    ++acks_;
    return RelayDeleteResult{};
  }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  /** Batches the app took in and acked: the coordinator acks only after the receive pipeline has taken a batch. */
  int Acks() {
    std::lock_guard lock(mu_);
    return acks_;
  }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest& request) override {
    ChatHistoryResponse response; // a gap repair's ask: the sender holds nothing in the range
    response.session_epoch = request.session_epoch;
    return response;
  }

private:
  std::mutex mu_;
  std::vector<RelayEnvelope> inbox_;
  int polls_ = 0;
  int acks_ = 0;
};

class RemovedChatRereadTest : public ::testing::Test {
protected:
  RemovedChatRereadTest()
      : dir_(std::filesystem::temp_directory_path() / ("pp_removed_reread_" + util::GenerateUuid())),
        identity_(dir_.string(), "test"),
        psk_(store_),
        roster_(store_),
        contacts_(dir_.string()),
        shadows_(directory_),
        labels_(contacts_, PeerDisplaySourcesFrom(shadows_)),
        inbox_(store_, contacts_, labels_, &shadows_),
        cards_(card_rows_),
        keys_(signing_, kem_, cards_, directory_),
        carol_psk_(carol_store_) {}

  void SetUp() override {
    EnsureSodiumInit();
    std::filesystem::create_directories(dir_);
    AppRuntime::Initialize(AppRuntimeConfig{});
    ASSERT_TRUE(identity_.SetDek(TestDek(3)) && psk_.SetDek(TestDek(3)) && store_.SetDek(TestDek(3)));
    ASSERT_TRUE(identity_.LoadOrCreate());
    me_ = *identity_.Get();
    carol_ = test::TestAccount::Make();
    ASSERT_TRUE(keys_.AddAccountCard(carol_.DeviceCard(), util::NowUnixMs(), "test"));
    ASSERT_TRUE(carol_store_.SetDek(TestDek(9)) && carol_psk_.SetDek(TestDek(9)));
    relay_ = std::make_shared<MailboxRelay>();
    StartApp();
  }

  /** Alice's messaging over her stores (again: a restart). */
  void StartApp() {
    mesh_ =
        std::make_unique<MeshDeliveryOrchestrator>(store_, contacts_, identity_, relay_, inbox_, keys_, psk_, roster_);
    mesh_->SetProfileDataDir(dir_.string());
    mesh_->SetRelayClients(MeshDeliveryOrchestrator::RelayClients{
        .preferred = relay_, .history = relay_, .sender = relay_.get(), .inboxes = {RelayInbox{"https://r", relay_}}});
    mesh_->SetOnInboundMessage([this](const std::string& id, const std::string&) { inbound_.push_back(id); });
    mesh_->SetOnBackgroundUnread(
        [this](const std::string&, const std::string&, const std::string&, bool) { ++notices_; });
  }

  void TearDown() override {
    (void)AppRuntime::DrainWorkersThenUI();
    mesh_.reset();
    AppRuntime::Shutdown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  /** Carol's message to this device, sealed in her pair with it (the next number there) and signed. */
  RelayEnvelope FromCarol(const std::string& text, const SendRelayOptions& options = {}) {
    ThreadMessage message = NewOutboundMessage("carol-chat", text, options, util::GenerateUuid(), util::NowUnixMs());
    const PeerSessionKey pair = GroupE2ePayloadCodec::PairTargetKey(me_.account_id, me_.peer_id);
    auto copy = SealPairCopy(message, carol_.account_id, options, carol_psk_, pair,
                             Base64Decode(me_.device_kem_public_key_b64).value());
    EXPECT_TRUE(copy) << copy.error().message;
    RelayEnvelope envelope = BuildDirectEnvelope(
        message, options, EnvelopeSender{carol_.account_id, carol_.device.peer_id, std::nullopt},
        ThreadChannel::E2ePublic, me_.account_id, {DeviceCopy(me_.peer_id, std::move(*copy))}, NextRelayOrderKey());
    test::SignEnvelopeAs(carol_, envelope);
    return envelope;
  }

  /** Carol deletes her message `target_id` for everyone (a retract annotation). */
  RelayEnvelope CarolRetracts(const std::string& target_id) {
    SendRelayOptions options;
    options.content_type = ChatContentType::Annotation;
    options.payload_json = BuildReactionPayloadJson(kAnnotationTypeRetract, target_id, "");
    return FromCarol("", options);
  }

  /** This device in group `group_id` with Carol, and its chat. */
  Thread JoinGroupWithCarol(const std::string& group_id) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = carol_.account_id;
    metadata.title = "group";
    metadata.roster_epoch = 1;
    EXPECT_TRUE(roster_.UpsertMetadata(metadata));
    for (const std::string& account : {carol_.account_id, me_.account_id}) {
      GroupRosterMember member;
      member.member_identity = account;
      member.contact_id = account;
      member.role = account == carol_.account_id ? MemberRole::Owner : MemberRole::Member;
      member.joined_at = 1;
      EXPECT_TRUE(roster_.UpsertMember(group_id, member));
    }
    auto thread = store_.FindOrCreateGroupThread(group_id, "group", {carol_.account_id, me_.account_id});
    EXPECT_TRUE(thread);
    return *thread;
  }

  /** Carol's message in group `group_id`, her copy for this account (as SendGroupMessage seals it). */
  RelayEnvelope CarolInGroup(const std::string& group_id, const std::string& text) {
    ThreadMessage message;
    message.id = util::GenerateUuid();
    message.text = text;
    message.timestamp = util::NowUnixMs();
    GroupMemberTarget target{me_.account_id, GroupE2ePayloadCodec::PairTargetKey(me_.account_id)};
    target.session_epoch = 1;
    target.sender_seq = ++group_seq_;
    const ByteVector my_kem = Base64Decode(me_.kem_public_key_b64).value();
    auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
        message.text, group_id, carol_.account_id, message.id, message.timestamp, {target}, carol_psk_,
        [my_kem](const PeerSessionKey&) -> Roe<ByteVector> { return my_kem; }, std::nullopt);
    EXPECT_TRUE(encrypted) << encrypted.error().message;
    const RelayCopy copy{.session_epoch = target.session_epoch,
                         .sender_seq = target.sender_seq,
                         .key_init_b64 = encrypted->copies.front()->key_init_b64,
                         .payload_b64 = encrypted->copies.front()->payload_b64};
    RelayEnvelope envelope =
        BuildGroupMemberEnvelope(message, SendRelayOptions{}, EnvelopeSender{carol_.account_id, carol_.device.peer_id},
                                 group_id, me_.account_id, {copy}, NextRelayOrderKey());
    test::SignEnvelopeAs(carol_, envelope);
    return envelope;
  }

  /** One poll that hands over `records`. */
  void Poll(const std::vector<RelayEnvelope>& records) {
    for (const RelayEnvelope& record : records) {
      relay_->Put(record);
    }
    const int acks_before = relay_->Acks();
    mesh_->SyncInboxFromWake(true);
    // The poll runs on a background worker and the batch is taken in on another, neither of which a drain waits for
    // (a loaded box can reach the drain first): wait for the outcome — the batch taken in and acked — then drain what
    // the ingest started.
    for (int i = 0; i < 10000 && relay_->Acks() == acks_before; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GT(relay_->Acks(), acks_before) << "the mailbox was never taken in";
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI()); // what the ingest started on its own worker turn
  }

  /** The app stopped, an older app wrote its relay cursor file again (no `registrations`) `age` ago, and it started. */
  void RestartWithAnOlderAppsCursorFile(const std::chrono::minutes age) {
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
    mesh_.reset();
    const auto path = dir_ / "relay_inbox_cursor.json";
    std::string text;
    { // closed before the file is replaced: Windows
      std::ifstream in(path);
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    auto root = TryParseObject(text);
    ASSERT_TRUE(root && root->contains("registrations") && root->contains("cursor")) << text;
    root->erase("registrations");
    {
      std::ofstream out(path, std::ios::trunc);
      out << DumpJson(*root);
    }
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() - age);
    StartApp();
  }

  std::optional<Thread> ChatWithCarol() {
    auto thread = store_.FindDirectThread(DirectChatTarget{.peer_identity_kind = "account",
                                                           .peer_identity_value = carol_.account_id,
                                                           .channel = ThreadChannel::E2ePublic});
    return thread && *thread ? std::optional<Thread>(**thread) : std::nullopt;
  }

  std::vector<ThreadMessage> Rows(const std::string& thread_id) {
    auto rows = store_.GetMessagesPage(thread_id, std::nullopt, 100);
    return rows ? *rows : std::vector<ThreadMessage>{};
  }

  int Unread(const std::string& thread_id) {
    auto thread = store_.GetThread(thread_id);
    return thread && *thread ? (*thread)->unread_count : -1;
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
  InMemoryDeliveryStore carol_store_;
  InMemoryPeerSessions carol_psk_;
  LocalIdentity me_;
  test::TestAccount carol_;
  uint64_t group_seq_ = 0;
  std::shared_ptr<MailboxRelay> relay_;
  std::unique_ptr<MeshDeliveryOrchestrator> mesh_;
  std::vector<std::string> inbound_;
  int notices_ = 0;
};

TEST_F(RemovedChatRereadTest, AMessageDeletedHereStaysDeletedWhenTheMailboxIsReadAgain) {
  const RelayEnvelope m1 = FromCarol("one");
  const RelayEnvelope m2 = FromCarol("two");
  Poll({m1, m2});
  const auto chat = ChatWithCarol();
  ASSERT_TRUE(chat);
  ASSERT_EQ(Rows(chat->id).size(), 2u);
  ASSERT_TRUE(inbox_.DeleteMessageForMe(chat->id, m1.message_id));
  inbox_.MarkThreadRead(chat->id);
  const size_t inbound_before = inbound_.size();
  const int notices_before = notices_;

  Poll({m1, m2}); // the mailbox from its start

  const std::vector<ThreadMessage> rows = Rows(chat->id);
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows.front().content_type, ChatContentType::Deleted) << "its tombstone stays";
  EXPECT_TRUE(rows.front().text.empty());
  EXPECT_EQ(inbound_.size(), inbound_before);
  EXPECT_EQ(notices_, notices_before);
  EXPECT_EQ(Unread(chat->id), 0);
}

TEST_F(RemovedChatRereadTest, AMessageItsSenderDeletedForEveryoneStaysDeleted) {
  const RelayEnvelope m1 = FromCarol("regret");
  const RelayEnvelope retract = CarolRetracts(m1.message_id);
  Poll({m1, retract});
  const auto chat = ChatWithCarol();
  ASSERT_TRUE(chat);
  auto row = store_.GetMessage(chat->id, m1.message_id);
  ASSERT_TRUE(row && *row);
  ASSERT_EQ((*row)->content_type, ChatContentType::Deleted);
  inbox_.MarkThreadRead(chat->id);
  const size_t inbound_before = inbound_.size();

  Poll({m1, retract});

  row = store_.GetMessage(chat->id, m1.message_id);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->content_type, ChatContentType::Deleted);
  EXPECT_TRUE((*row)->text.empty());
  EXPECT_EQ(inbound_.size(), inbound_before);
  EXPECT_EQ(Unread(chat->id), 0);
}

TEST_F(RemovedChatRereadTest, AClearedChatGetsNothingBackAndANewMessageStillArrives) {
  const RelayEnvelope m1 = FromCarol("one");
  const RelayEnvelope m2 = FromCarol("two");
  Poll({m1, m2});
  const auto chat = ChatWithCarol();
  ASSERT_TRUE(chat);
  ASSERT_TRUE(inbox_.ClearThreadHistory(chat->id, false));
  ASSERT_TRUE(Rows(chat->id).empty());
  const size_t inbound_before = inbound_.size();
  const int notices_before = notices_;

  Poll({m1, m2});

  EXPECT_TRUE(Rows(chat->id).empty()) << "the device pair's numbering would take them as late ones: dropped first";
  EXPECT_EQ(inbound_.size(), inbound_before);
  EXPECT_EQ(notices_, notices_before);
  EXPECT_EQ(Unread(chat->id), 0);

  const RelayEnvelope m3 = FromCarol("after the clear");
  Poll({m3});
  const std::vector<ThreadMessage> rows = Rows(chat->id);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().id, m3.message_id);
  EXPECT_EQ(inbound_.size(), inbound_before + 1);
}

TEST_F(RemovedChatRereadTest, ADeletedPublicChatIsNotMadeAgainAndANewMessageMakesIt) {
  const RelayEnvelope m1 = FromCarol("one");
  Poll({m1});
  const auto chat = ChatWithCarol();
  ASSERT_TRUE(chat);
  ASSERT_TRUE(inbox_.CloseThread(chat->id));
  ASSERT_FALSE(ChatWithCarol());
  const size_t inbound_before = inbound_.size();
  const int notices_before = notices_;

  Poll({m1});

  EXPECT_FALSE(ChatWithCarol()) << "no chat made again for a message of the deleted one";
  EXPECT_EQ(inbound_.size(), inbound_before);
  EXPECT_EQ(notices_, notices_before);

  const RelayEnvelope m2 = FromCarol("hello again");
  Poll({m2});
  const auto again = ChatWithCarol();
  ASSERT_TRUE(again) << "a new message makes the chat";
  const std::vector<ThreadMessage> rows = Rows(again->id);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().id, m2.message_id);
  EXPECT_EQ(inbound_.size(), inbound_before + 1);
}

// A chat cleared under an older app has no removed ids kept. When that app's cursor file (it noted no registrations)
// makes the inbox be read again from its start, what the relay stored before the file was last written is skipped: the
// chat does not fill again, while a message stored since still arrives.
TEST_F(RemovedChatRereadTest, AChatClearedUnderAnOlderAppDoesNotFillAgainWhenItsCursorFileIsReadAgain) {
  const int64_t stored_long_ago = util::NowUnixMs() - 3 * 3600 * 1000;
  RelayEnvelope m1 = FromCarol("one");
  RelayEnvelope m2 = FromCarol("two");
  m1.relay_created_at_ms = stored_long_ago;
  m2.relay_created_at_ms = stored_long_ago;
  Poll({m1, m2});
  const auto chat = ChatWithCarol();
  ASSERT_TRUE(chat);
  ASSERT_EQ(Rows(chat->id).size(), 2u);
  ASSERT_TRUE(inbox_.ClearThreadHistory(chat->id, false));
  store_.ForgetRemovedIds(); // the older app kept none
  RestartWithAnOlderAppsCursorFile(std::chrono::hours(1));
  const size_t inbound_before = inbound_.size();

  RelayEnvelope m3 = FromCarol("after the update");
  m3.relay_created_at_ms = util::NowUnixMs();
  Poll({m1, m2, m3}); // the mailbox from its start, and one new

  const std::vector<ThreadMessage> rows = Rows(chat->id);
  ASSERT_EQ(rows.size(), 1u) << "the cleared messages stay gone";
  EXPECT_EQ(rows.front().id, m3.message_id);
  EXPECT_EQ(inbound_.size(), inbound_before + 1);
  EXPECT_EQ(Unread(chat->id), 1);
}

TEST_F(RemovedChatRereadTest, AClearedGroupChatGetsNothingBackAndANewMessageStillArrives) {
  const Thread group = JoinGroupWithCarol("g1");
  const RelayEnvelope g1 = CarolInGroup("g1", "one");
  const RelayEnvelope g2 = CarolInGroup("g1", "two");
  Poll({g1, g2});
  ASSERT_EQ(Rows(group.id).size(), 2u);
  ASSERT_TRUE(inbox_.ClearThreadHistory(group.id, false));
  const size_t inbound_before = inbound_.size();

  Poll({g1, g2});

  EXPECT_TRUE(Rows(group.id).empty()) << "a group keeps no cleared-history floor: dropped by id";
  EXPECT_EQ(inbound_.size(), inbound_before);
  EXPECT_EQ(Unread(group.id), 0);

  const RelayEnvelope g3 = CarolInGroup("g1", "after the clear");
  Poll({g3});
  const std::vector<ThreadMessage> rows = Rows(group.id);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().id, g3.message_id);
}

TEST_F(RemovedChatRereadTest, ADeletedGroupChatIsNotMadeAgain) {
  const Thread group = JoinGroupWithCarol("g2");
  const RelayEnvelope g1 = CarolInGroup("g2", "one");
  Poll({g1});
  ASSERT_EQ(Rows(group.id).size(), 1u);
  ASSERT_TRUE(inbox_.CloseThread(group.id));
  const size_t inbound_before = inbound_.size();

  Poll({g1});

  auto thread = store_.FindGroupThread("g2");
  ASSERT_TRUE(thread);
  EXPECT_FALSE(*thread);
  EXPECT_EQ(inbound_.size(), inbound_before);
}

// ---- our other device's sent copy: its record names another id (the notice's), the message id is inside -------------

/** This device and our other one (one account), the receive pipeline only, as message_edit_pipeline_test has them. */
class RemovedChatSentCopyTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    me_ = test::TestAccount::Make();
    other_device_ = test::TestDevice::Make();
    peer_ = test::TestAccount::Make();
    ASSERT_TRUE(store_.SetDek(TestDek(5)) && psk_.SetDek(TestDek(5)) && store_.ListThreads());
    AccountCard own{.keys = me_.Card(), .nickname = "test", .issued_at = 1};
    own.devices = {me_.device.Entry(), other_device_.Entry()};
    cards_[me_.account_id] = SignAccountCard(own, [this](const ByteVector& bytes) { return me_.Sign(bytes); }).value();
    pipeline_ = std::make_unique<RelayReceivePipeline>(
        store_,
        [this](const std::string& account) -> std::optional<AccountCard> {
          auto found = cards_.find(account);
          return found == cards_.end() ? std::nullopt : std::optional<AccountCard>(found->second);
        },
        psk_, test::ReceiveKeysOf(me_), roster_);
  }
  void TearDown() override { pipeline_.reset(); }

  /** Our message `sent` to the peer, as our other device mirrors it to this one (ControlLane::NotifySiblings). */
  RelayEnvelope SentCopy(const ThreadMessage& sent) {
    const ThreadMessage notice = sent_copy::BuildNotice(sent, SentCopyTarget{.peer_account = peer_.account_id}).value();
    auto opened = AutoKeyEstablishment::EncapsulateForRecipient(me_.kem_public);
    EXPECT_TRUE(opened);
    ControlLaneSendParams params;
    params.sender_account_id = me_.account_id;
    params.sender_device = other_device_.peer_id;
    params.peer_account_id = me_.account_id;
    params.copies = {
        ControlLaneCopy{.session_epoch = 1, .master_psk = opened->master_psk, .key_init_b64 = opened->key_init_b64}};
    params.order_key = NextRelayOrderKey();
    auto envelope = BuildControlLaneEnvelope(
        params, notice, [this](const std::vector<uint8_t>& bytes) { return other_device_.Sign(bytes); });
    EXPECT_TRUE(envelope) << envelope.error().message;
    return *envelope;
  }

  std::optional<Thread> ChatWithPeer() {
    auto thread = store_.FindDirectThread(DirectChatTarget{
        .peer_identity_kind = "account", .peer_identity_value = peer_.account_id, .channel = ThreadChannel::E2ePublic});
    return thread && *thread ? std::optional<Thread>(**thread) : std::nullopt;
  }

  InMemoryDeliveryStore store_;
  InMemoryPeerSessions psk_{store_};
  InMemoryGroupRosterStore roster_{store_};
  std::map<std::string, AccountCard> cards_;
  test::TestAccount me_;
  test::TestDevice other_device_;
  test::TestAccount peer_;
  std::unique_ptr<RelayReceivePipeline> pipeline_;
};

TEST_F(RemovedChatSentCopyTest, OurOtherDevicesCopyOfAMessageInAClearedChatIsNotFiledAgain) {
  ThreadMessage sent;
  sent.id = util::GenerateUuid();
  sent.text = "sent from the other device";
  sent.timestamp = util::NowUnixMs();
  const RelayEnvelope copy = SentCopy(sent);
  ASSERT_NE(copy.message_id, sent.id) << "the record names the notice, not the message";
  RelayReceiveOutcome first = pipeline_->ProcessEnvelope(copy, me_.account_id);
  ASSERT_TRUE(first.persisted);
  const auto chat = ChatWithPeer();
  ASSERT_TRUE(chat);
  ASSERT_TRUE(store_.ClearMessages(chat->id, ClearMessagesOptions{}));

  const RelayReceiveOutcome again = pipeline_->ProcessEnvelope(copy, me_.account_id);

  EXPECT_FALSE(again.persisted);
  auto rows = store_.GetMessagesPage(chat->id, std::nullopt, 10);
  ASSERT_TRUE(rows);
  EXPECT_TRUE(rows->empty()) << "the store refuses an id it remembers as removed";
}

TEST_F(RemovedChatSentCopyTest, OurOtherDevicesCopyForADeletedChatFilesNothing) {
  ThreadMessage sent;
  sent.id = util::GenerateUuid();
  sent.text = "sent from the other device";
  sent.timestamp = util::NowUnixMs();
  const RelayEnvelope copy = SentCopy(sent);
  ASSERT_TRUE(pipeline_->ProcessEnvelope(copy, me_.account_id).persisted);
  const auto chat = ChatWithPeer();
  ASSERT_TRUE(chat);
  ASSERT_TRUE(store_.DeleteThread(chat->id));

  const RelayReceiveOutcome again = pipeline_->ProcessEnvelope(copy, me_.account_id);

  EXPECT_FALSE(again.persisted);
  // The pipeline opens the chat before it files the copy (FileSentCopy): an empty one, not listed, may be left.
  if (const auto reopened = ChatWithPeer()) {
    auto rows = store_.GetMessagesPage(reopened->id, std::nullopt, 10);
    ASSERT_TRUE(rows);
    EXPECT_TRUE(rows->empty());
    EXPECT_FALSE(reopened->listed);
  }
}

} // namespace
} // namespace pbr
