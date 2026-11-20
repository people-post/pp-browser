// A newer app's message on the app's own storage (release-compatibility INVENTORY T1, T3): the receive pipeline keeps
// an unknown content type on SQLite as received, shows it as "update to view", consumes its seq, and the row reads
// back the same after a restart. Before, the store refused it ("Cannot encode unsupported ChatPayload") and the seq
// stalled.
#include "lib/storage/SqliteThreadStore.h"

#include "common/Serialize.hpp"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kPeer = "account:peer";

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x50 + i);
  }
  return dek;
}

ByteVector TestMasterPsk() {
  return *HexToBytes("000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");
}

std::vector<uint8_t> PayloadBytes(uint8_t version, uint8_t type, const std::string& text) {
  std::ostringstream oss;
  {
    OutputArchive ar(oss);
    ar & version;
    ar & type;
    WireLenUtf8 field{text};
    ar & field;
  }
  const std::string data = oss.str();
  return std::vector<uint8_t>(data.begin(), data.end());
}

class TolerantReceiveSqliteTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_storage_tolerant_" + util::GenerateUuid());
    Open();
    ASSERT_TRUE(side_.SetDek(TestDek()));
    ASSERT_TRUE(psk_store_.SetDek(TestDek()));
    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = kPeer;
    target.channel = ThreadChannel::E2e;
    auto thread = store_->FindOrCreateDirectThread(target, "contact-peer", "Peer");
    ASSERT_TRUE(thread);
    thread_id_ = thread->id;
    PeerSessionRecord record;
    record.key = E2eRelayPayloadCodec::ChatTargetFromThread(*thread);
    record.session_epoch = 1;
    record.master_psk_b64 = Base64Encode(TestMasterPsk());
    record.psk_verified_at = 1;
    ASSERT_TRUE(psk_store_.Save(record));
  }

  void TearDown() override {
    pipeline_.reset();
    store_.reset(); // before the directory goes (Windows holds open files)
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  void Open() {
    pipeline_.reset();
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->SetDek(TestDek()));
    pipeline_ = std::make_unique<RelayReceivePipeline>(
        *store_,
        [card = peer_.DeviceCard()](const std::string& account) -> std::optional<AccountCard> {
          return account == kPeer ? std::optional<AccountCard>(card) : std::nullopt;
        },
        psk_store_, test::ReceiveKeysOf(local_), roster_store_);
  }

  RelayEnvelope Envelope(const uint64_t seq, const std::vector<uint8_t>& plaintext) {
    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = "peer-msg-" + std::to_string(seq);
    envelope.sender_contact_id = kPeer;
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2e;
    envelope.sender_seq = seq;
    envelope.session_epoch = 1;
    envelope.timestamp = static_cast<int64_t>(1000 + seq);
    E2eEncryptParams params;
    params.channel = CryptoChannel::E2e;
    params.peer_contact_id = local_.account_id;
    params.sender_contact_id = kPeer;
    params.message_id = envelope.message_id;
    params.sender_seq = seq;
    params.session_epoch = 1;
    params.timestamp = envelope.timestamp;
    envelope.body.e2e.payload_b64 =
        E2eRelayPayloadCodec::EncryptChatPayloadBytes(params, plaintext, TestMasterPsk()).value();
    test::SignEnvelopeAs(peer_, envelope);
    return envelope;
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  InMemoryDeliveryStore side_; // the pair's session and the (unused) roster
  InMemoryPeerSessions psk_store_{side_};
  InMemoryGroupRosterStore roster_store_{side_};
  test::TestAccount local_ = test::TestAccount::Make();
  test::TestAccount peer_ = test::TestAccount::Make();
  std::unique_ptr<RelayReceivePipeline> pipeline_;
  std::string thread_id_;
};

TEST_F(TolerantReceiveSqliteTest, AnUnknownContentTypeIsStoredAsReceivedAndItsSeqConsumed) {
  const std::vector<uint8_t> newer = PayloadBytes(1, 42, "a poll");
  const RelayReceiveOutcome outcome = pipeline_->ProcessEnvelope(Envelope(1, newer), local_.account_id);
  ASSERT_TRUE(outcome.persisted) << outcome.receive_failure_detail;
  ASSERT_TRUE(outcome.receive_failure);
  EXPECT_EQ(*outcome.receive_failure, ReceiveFailure::NeedsUpdate);
  EXPECT_EQ(store_->GetPeerSyncState(thread_id_, 1)->contiguous_peer_seq, 1u);

  const RelayReceiveOutcome next =
      pipeline_->ProcessEnvelope(Envelope(2, *ChatPayloadCodec::EncodeText("hello")), local_.account_id);
  EXPECT_EQ(next.decision, IngestDecision::AcceptContiguous) << "no stall behind it";

  store_->Flush();
  Open(); // a restart: the rows come back from disk
  auto rows = store_->GetMessages(thread_id_);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 2u);
  const ThreadMessage& kept = rows->at(0).id == "peer-msg-1" ? rows->at(0) : rows->at(1);
  EXPECT_EQ(kept.content_type, ChatContentType::Unsupported);
  auto row = ChatPayloadCodec::EncodeToRow(kept);
  ASSERT_TRUE(row);
  EXPECT_EQ(*row, newer) << "the bytes as received, for the updated app to read";
  EXPECT_EQ(pipeline_->ProcessEnvelope(Envelope(1, newer), local_.account_id).decision, IngestDecision::BenignDuplicate)
      << "a redelivery is a duplicate, not a second row";
}

TEST_F(TolerantReceiveSqliteTest, AHigherPayloadVersionIsKeptOnDiskToo) {
  const RelayReceiveOutcome outcome =
      pipeline_->ProcessEnvelope(Envelope(1, PayloadBytes(2, 0, "v2")), local_.account_id);
  ASSERT_TRUE(outcome.persisted) << outcome.receive_failure_detail;
  EXPECT_EQ(outcome.receive_failure.value_or(ReceiveFailure::DecodeFailed), ReceiveFailure::NeedsUpdate);
  store_->Flush();
  Open();
  auto rows = store_->GetMessages(thread_id_);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ(rows->front().content_type, ChatContentType::Unsupported);
}

} // namespace
} // namespace pbr
