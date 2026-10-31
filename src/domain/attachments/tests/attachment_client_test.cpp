#include "domain/attachments/AttachmentClient.h"

#include "domain/attachments/ChatBlobRequest.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/error/AppError.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/IdentityStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x30 + i);
  }
  return dek;
}

/** A peer-direct client whose answers and calls the test controls. */
class FakePeerClient : public IChatBlobPeerClient {
public:
  bool reachable = true;
  bool push_ok = true;
  int push_calls = 0;
  size_t pushed_bytes = 0;

  bool IsPeerReachable(const std::string&) const override { return reachable; }
  Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest&) override { return Error("not used"); }
  Roe<void> PushChatBlob(const ChatBlobRequest&, const std::vector<uint8_t>& ciphertext) override {
    ++push_calls;
    pushed_bytes = ciphertext.size();
    if (!push_ok) {
      return Error("peer did not answer");
    }
    return {};
  }
};

/** MockBlobClient that remembers which tier the server put the last upload in. */
class TierRecordingBlob : public MockBlobClient {
public:
  Roe<BlobPresignResult> Presign(const std::string& relay_user_id, const std::string& content_type,
                                 const uint64_t byte_length, const BlobPurpose purpose) override {
    auto result = MockBlobClient::Presign(relay_user_id, content_type, byte_length, purpose);
    if (result) {
      last_tier = result->tier;
    }
    return result;
  }
  BlobTier last_tier = BlobTier::Small;
};

class AttachmentClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_client_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    ASSERT_TRUE(identity_->LoadOrCreate());
    LocalIdentity updated = *identity_->Get();
    updated.relay_user_id = "relay:local";
    updated.registered = true;
    ASSERT_TRUE(identity_->Update(updated));

    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    Contact peer;
    peer.id = "contact-peer";
    peer.remote.ids.push_back(mesh::ContactId{.kind = mesh::ContactIdKind::RelayUser, .value = "relay:peer"});
    peer.local.display_name = "Peer";
    SyncContactMirrors(peer);
    ASSERT_TRUE(contacts_->Upsert(peer));

    direct_.id = "thread-direct";
    direct_.kind = ThreadKind::Direct;
    direct_.channel = ThreadChannel::E2e;
    direct_.participant_contact_ids = {"contact-peer"};
    direct_.peer_identity_kind = "account";
    direct_.peer_identity_value = "account-peer";

    group_ = direct_;
    group_.id = "thread-group";
    group_.kind = ThreadKind::Group;
    group_.group_id = "g1";

    // The fixture must be a thread the direct path can address, or "falls back" would pass for the wrong reason.
    const auto request = BuildChatBlobRequest(direct_, *contacts_, *identity_, ChatBlobOp::Push, direct_.id,
                                              std::vector<uint8_t>(kAttachmentContentHashSize, 1));
    ASSERT_TRUE(request) << request.error().message;
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  ChatAttachmentUploadOptions Options(const Thread& thread) {
    ChatAttachmentUploadOptions options;
    options.peer_client = &peer_;
    options.contacts = contacts_.get();
    options.thread = thread;
    options.thread_id = thread.id;
    return options;
  }

  Roe<ChatAttachmentFields> Upload(const size_t size, ChatAttachmentUploadOptions options) {
    Roe<ChatAttachmentFields> result = Error("not called");
    const std::vector<uint8_t> bytes(size, 0x42);
    UploadChatAttachmentAsync(blob_, *identity_, bytes, "doc.pdf", "", std::move(options),
                              [&result](Roe<ChatAttachmentFields> value) { result = std::move(value); });
    return result;
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<ContactsStore> contacts_;
  Thread direct_;
  Thread group_;
  FakePeerClient peer_;
  TierRecordingBlob blob_;
};

TEST_F(AttachmentClientTest, SmallFileToAReachableContactGoesDirectAndNotToTheServer) {
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_.PresignCallCount(), 0u);
  EXPECT_TRUE(fields->url.empty()) << "a direct push leaves no server copy";
  EXPECT_EQ(fields->mime, "application/pdf");
}

TEST_F(AttachmentClientTest, DirectFailureFallsBackToTheServer) {
  peer_.push_ok = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1) << "direct was tried first";
  EXPECT_EQ(blob_.PresignCallCount(), 1u);
  ASSERT_EQ(blob_.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_.RetainedBlobIds().size(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, UnreachableContactGoesStraightToTheServer) {
  peer_.reachable = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_.PresignCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, GroupAttachmentsAreServerOnly) {
  auto fields = Upload(1000, Options(group_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "no peer push in a group, even with a reachable member";
  EXPECT_EQ(blob_.PresignCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, AboveTheDirectLimitSkipsDirectAndGetsTheLargeTier) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL; // a raised limit, as step 2 will ship
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1, options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "bigger than kMaxDirectAttachmentBytes never tries the 15 s direct path";
  EXPECT_EQ(blob_.PresignCallCount(), 1u);
  EXPECT_EQ(blob_.last_tier, BlobTier::Large);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, ExactlyTheDirectLimitStillGoesDirect) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL;
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes), options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_.PresignCallCount(), 0u);
}

TEST_F(AttachmentClientTest, OverTheCallersLimitFailsBeforeAnyNetwork) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 999;
  auto fields = Upload(1000, options);
  EXPECT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_.PresignCallCount(), 0u);
}

TEST_F(AttachmentClientTest, ServerErrorsKeepTheirTypeToTheCaller) {
  peer_.reachable = false;
  blob_.SetPresignError(AppError::Blob(Err::Blob::QuotaExceeded, "HTTP 429: Small file quota exceeded"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::QuotaExceeded));
}

TEST(AttachmentRoutingTest, OnlyAnE2eDirectThreadUpToTheLimitTriesDirect) {
  Thread direct;
  direct.kind = ThreadKind::Direct;
  direct.channel = ThreadChannel::E2e;
  Thread group = direct;
  group.kind = ThreadKind::Group;
  EXPECT_TRUE(AttachmentTriesPeerDirect(direct, 1));
  EXPECT_TRUE(AttachmentTriesPeerDirect(direct, kMaxDirectAttachmentBytes));
  EXPECT_FALSE(AttachmentTriesPeerDirect(direct, kMaxDirectAttachmentBytes + 1));
  EXPECT_FALSE(AttachmentTriesPeerDirect(group, 1));
  EXPECT_FALSE(AttachmentTriesPeerDirect(std::nullopt, 1));
}

} // namespace
