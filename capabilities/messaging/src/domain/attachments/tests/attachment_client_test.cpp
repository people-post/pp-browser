#include "lib/delivery/RelayClients.h"
#include "domain/attachments/AttachmentClient.h"

#include "domain/attachments/ChatBlobRequest.h"
#include "lib/delivery/AttachmentContentCipher.h"
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
  std::function<void()> on_push;

  bool IsPeerReachable(const std::string&) const override { return reachable; }
  Roe<std::vector<uint8_t>> FetchChatBlob(const ChatBlobRequest&) override { return Error("not used"); }
  Roe<void> PushChatBlob(const ChatBlobRequest&, const std::vector<uint8_t>& ciphertext) override {
    ++push_calls;
    pushed_bytes = ciphertext.size();
    if (on_push) {
      on_push();
    }
    if (!push_ok) {
      return Error("peer did not answer");
    }
    return {};
  }
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
  MockBlobClient blob_;
};

TEST_F(AttachmentClientTest, SmallFileToAReachableContactGoesDirectAndNotToTheServer) {
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_.FileStartCallCount(), 0u);
  EXPECT_TRUE(fields->url.empty()) << "a direct push leaves no server copy";
  EXPECT_EQ(fields->mime, "application/pdf");
}

TEST_F(AttachmentClientTest, DirectFailureFallsBackToTheServer) {
  peer_.push_ok = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1) << "direct was tried first";
  EXPECT_EQ(blob_.FileStartCallCount(), 1u);
  ASSERT_EQ(blob_.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_.CompletedFileIds().size(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, UnreachableContactGoesStraightToTheServer) {
  peer_.reachable = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_.FileStartCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, GroupAttachmentsAreServerOnly) {
  auto fields = Upload(1000, Options(group_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "no peer push in a group, even with a reachable member";
  EXPECT_EQ(blob_.FileStartCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, AboveTheDirectLimitSkipsDirectAndGoesToTheServer) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL; // a raised limit, as step 2 will ship
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1, options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "bigger than kMaxDirectAttachmentBytes never tries the 15 s direct path";
  ASSERT_EQ(blob_.FileStartCallCount(), 1u);
  EXPECT_EQ(blob_.FileStartByteLengths().front(), kMaxDirectAttachmentBytes + 1 + kAttachmentCipherOverheadBytes)
      << "the relay reserves the ciphertext length";
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, ExactlyTheDirectLimitStillGoesDirect) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL;
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes), options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_.FileStartCallCount(), 0u);
}

TEST_F(AttachmentClientTest, OverTheCallersLimitFailsBeforeAnyNetwork) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 999;
  auto fields = Upload(1000, options);
  EXPECT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_.FileStartCallCount(), 0u);
}

TEST_F(AttachmentClientTest, ServerErrorsKeepTheirTypeToTheCaller) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::QuotaExceeded,
                                         "HTTP 429 QUOTA_FILE_BYTES: File storage quota exceeded byte_length=1016"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::QuotaExceeded));
}

TEST_F(AttachmentClientTest, NotConfiguredServerAfterAFailedDirectPushSaysBoth) {
  peer_.reachable = false; // no live direct link: the push fails before it is sent
  blob_.SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
  EXPECT_EQ(blob_.FileStartCallCount(), 1u);
}

TEST_F(AttachmentClientTest, NotConfiguredServerAfterARefusedDirectPushSaysBoth) {
  peer_.push_ok = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
}

TEST_F(AttachmentClientTest, NotConfiguredServerInAGroupKeepsTheServerOnlyMessage) {
  blob_.SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(group_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfigured));
}

TEST_F(AttachmentClientTest, OtherServerErrorsAfterAFailedDirectPushKeepTheirType) {
  peer_.push_ok = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::Unavailable));
}

// Review (a): a send that is not staged (no profile key) honours the cancel too: nothing goes after it.
TEST_F(AttachmentClientTest, AnUnstagedSendCancelledBeforeItStartsTouchesNoNetwork) {
  auto options = Options(direct_);
  options.cancelled = std::make_shared<std::atomic<bool>>(true);
  auto result = Upload(1000, std::move(options));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "Attachment send cancelled");
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_.FileStartCallCount(), 0u);
}

TEST_F(AttachmentClientTest, AnUnstagedSendCancelledDuringItsPushSendsNothing) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  peer_.push_ok = false; // the push fails: the server would be next
  peer_.on_push = [cancelled] { cancelled->store(true); };
  auto options = Options(direct_);
  options.cancelled = cancelled;
  auto result = Upload(1000, std::move(options));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "Attachment send cancelled");
  EXPECT_EQ(blob_.DeletedFileIds().size(), blob_.UploadedBodies().size()) << "whatever went up is deleted";
}

// The staged (product) path: the ciphertext is a file, the key is in the sealed record, and a failed send is
// resumed with that same ciphertext and key.
class StagedUploadTest : public AttachmentClientTest {
protected:
  void SetUp() override {
    AttachmentClientTest::SetUp();
    store_ = std::make_unique<OutgoingAttachmentStore>(dir_.string(), "profile-1");
  }
  ChatAttachmentUploadOptions Staged(const Thread& thread) {
    auto options = Options(thread);
    options.outgoing = store_.get();
    options.dek = TestDek();
    options.on_staged = [this](const OutgoingAttachmentRecord& record) { staged_ = record; };
    options.progress = [this](const uint64_t done, const uint64_t total) {
      progress_.emplace_back(done, total);
      return true;
    };
    return options;
  }
  Roe<ChatAttachmentFields> UploadStaged(const size_t size, ChatAttachmentUploadOptions options) {
    Roe<ChatAttachmentFields> result = Error("not called");
    const std::vector<uint8_t> bytes(size, 0x42);
    UploadChatAttachmentAsync(blob_, *identity_, bytes, "clip.mp4", "", std::move(options),
                              [&result](Roe<ChatAttachmentFields> value) { result = std::move(value); });
    return result;
  }
  std::unique_ptr<OutgoingAttachmentStore> store_;
  OutgoingAttachmentRecord staged_;
  std::vector<std::pair<uint64_t, uint64_t>> progress_;
};

TEST_F(StagedUploadTest, TheServerUploadStreamsTheStagedFileAndReportsProgress) {
  peer_.reachable = false;
  auto fields = UploadStaged(5000, Staged(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  ASSERT_FALSE(staged_.id.empty());
  auto cipher = store_->ReadCipher(staged_.id);
  ASSERT_TRUE(cipher);
  ASSERT_EQ(blob_.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_.UploadedBodies().front(), std::string(cipher->begin(), cipher->end()))
      << "what went out is the staged file, byte for byte";
  EXPECT_FALSE(progress_.empty());
  EXPECT_EQ(progress_.back().first, progress_.back().second);
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
  EXPECT_EQ(record->fields.url, fields->url);
}

// The hub frees the plaintext in on_staged (a 50 MB file is not held twice while it uploads): nothing after
// staging may read it, and the upload is still the staged file, byte for byte.
TEST_F(StagedUploadTest, ThePlaintextCanBeFreedOnceStagedAndTheUploadStillGoesOut) {
  peer_.reachable = false;
  std::vector<uint8_t> bytes(5000, 0x42);
  auto options = Staged(direct_);
  options.on_staged = [this, &bytes](const OutgoingAttachmentRecord& record) {
    staged_ = record;
    std::vector<uint8_t>().swap(bytes);
  };
  Roe<ChatAttachmentFields> result = Error("not called");
  UploadChatAttachmentAsync(blob_, *identity_, bytes, "clip.mp4", "", std::move(options),
                            [&result](Roe<ChatAttachmentFields> value) { result = std::move(value); });
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(bytes.empty());
  auto cipher = store_->ReadCipher(staged_.id);
  ASSERT_TRUE(cipher);
  ASSERT_EQ(blob_.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_.UploadedBodies().front(), std::string(cipher->begin(), cipher->end()));
}

TEST_F(StagedUploadTest, AFailedUploadKeepsTheCiphertextAndAResumeSendsTheSameBytesWithTheSameKey) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  auto failed = UploadStaged(5000, Staged(direct_));
  ASSERT_FALSE(failed);
  ASSERT_FALSE(staged_.id.empty());
  EXPECT_TRUE(store_->HasCipher(staged_.id)) << "kept for Retry";
  const auto cipher_before = *store_->ReadCipher(staged_.id);
  const auto key_before = staged_.fields.content_key;
  auto sealed = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(sealed);
  EXPECT_FALSE(sealed->delivered);

  blob_.ClearFileStartError();
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *sealed, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  ASSERT_TRUE(resumed) << resumed.error().message;
  EXPECT_EQ(resumed->content_key, key_before) << "same key: the message the recipient gets still decrypts";
  ASSERT_EQ(blob_.UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_.UploadedBodies().front(), std::string(cipher_before.begin(), cipher_before.end()));
  EXPECT_FALSE(resumed->url.empty());
}

TEST_F(StagedUploadTest, NotConfiguredServerAfterAFailedDirectPushSaysBothAndAGroupDoesNot) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto direct = UploadStaged(5000, Staged(direct_));
  ASSERT_FALSE(direct);
  EXPECT_EQ(direct.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
  auto group = UploadStaged(5000, Staged(group_));
  ASSERT_FALSE(group);
  EXPECT_EQ(group.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfigured));
}

TEST_F(StagedUploadTest, ADeliveredRecordResumesWithoutAnyNetwork) {
  peer_.reachable = false;
  auto fields = UploadStaged(5000, Staged(direct_));
  ASSERT_TRUE(fields);
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  const auto starts = blob_.FileStartCallCount();
  Roe<ChatAttachmentFields> again = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { again = std::move(value); });
  ASSERT_TRUE(again);
  EXPECT_EQ(again->url, fields->url);
  EXPECT_EQ(blob_.FileStartCallCount(), starts);
}

// A Retry of a record the contact already has (the process died before the message was made): a cancel is too late,
// the record stays and the fields come back so the message is created.
TEST_F(StagedUploadTest, ACancelledResumeOfADeliveredRecordStillGivesTheFields) {
  auto fields = UploadStaged(5000, Staged(direct_));
  ASSERT_TRUE(fields);
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  ASSERT_TRUE(record->delivered);
  auto options = Staged(direct_);
  options.cancelled = std::make_shared<std::atomic<bool>>(true);
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, std::move(options),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  ASSERT_TRUE(resumed) << resumed.error().message;
  EXPECT_TRUE(store_->Load(staged_.id, TestDek()));
}

TEST_F(StagedUploadTest, ADirectPushIsRecordedAsDeliveredWithNoUrl) {
  auto fields = UploadStaged(5000, Staged(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_TRUE(fields->url.empty());
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
}

TEST_F(StagedUploadTest, ALargeStagedFileSkipsTheDirectPathAndIsNeverHeldInMemory) {
  auto options = Staged(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL;
  auto fields = UploadStaged(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1, options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(staged_.cipher_bytes, kMaxDirectAttachmentBytes + 1 + kAttachmentCipherOverheadBytes);
  ASSERT_EQ(blob_.FileStartCallCount(), 1u);
  EXPECT_EQ(blob_.FileStartByteLengths().front(), staged_.cipher_bytes) << "ciphertext length, not the file's";
}

TEST_F(StagedUploadTest, ResumeWithoutTheCiphertextTellsTheCallerToPickAgain) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  std::filesystem::remove(store_->CipherPath(staged_.id));  // cleaned after a day, or lost
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  EXPECT_FALSE(resumed);
}

// Cancel on the chip while the file is going up: the upload is told to stop (a real PUT aborts mid-way; the mock's
// single PUT has already taken the bytes), any copy the server holds is deleted, the result is "cancelled" so the
// caller sends no message, and the staged copy is gone (no Retry chip comes back).
TEST_F(StagedUploadTest, ACancelDuringTheUploadSendsNothingAndLeavesNothingBehind) {
  peer_.reachable = false;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto options = Staged(direct_);
  options.cancelled = cancelled;
  bool stop_asked = false;
  options.progress = [cancelled, &stop_asked](uint64_t, uint64_t) {
    stop_asked = cancelled->load();
    cancelled->store(true); // the person taps Cancel once the upload has started
    return true;
  };
  auto result = UploadStaged(5000, std::move(options));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().message, "Attachment send cancelled");
  EXPECT_FALSE(stop_asked) << "the caller's progress is not asked again once cancelled";
  EXPECT_EQ(blob_.DeletedFileIds().size(), blob_.UploadedBodies().size()) << "whatever went up is deleted";
  ASSERT_FALSE(staged_.id.empty());
  EXPECT_FALSE(store_->HasCipher(staged_.id));
  EXPECT_TRUE(store_->ListForThread(direct_.id, TestDek()).empty());
}

// A cancel while a small file is pushed straight to the contact, and the push fails: no message, nothing left.
TEST_F(StagedUploadTest, ACancelDuringAFailingDirectPushSendsNothing) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  peer_.push_ok = false;
  peer_.on_push = [cancelled] { cancelled->store(true); };
  auto options = Staged(direct_);
  options.cancelled = cancelled;
  auto result = UploadStaged(5000, std::move(options));
  ASSERT_FALSE(result);
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_TRUE(blob_.UploadedBodies().empty());
  EXPECT_TRUE(store_->ListForThread(direct_.id, TestDek()).empty());
}

// A cancel that lands after the push reached the contact is too late: the send stands (fields come back, nothing is
// deleted, the record stays delivered); the caller tells the person it was already sent.
TEST_F(StagedUploadTest, ACancelAfterASuccessfulDirectPushIsTooLate) {
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  peer_.on_push = [cancelled] { cancelled->store(true); };
  auto options = Staged(direct_);
  options.cancelled = cancelled;
  auto result = UploadStaged(5000, std::move(options));
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_TRUE(result->url.empty());
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_TRUE(blob_.DeletedFileIds().empty());
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
}

// A cancel during a staged Retry only stops that attempt: the record stays, so Retry still works.
TEST_F(StagedUploadTest, ACancelledResumeKeepsTheRecordForRetry) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  blob_.ClearFileStartError();
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto options = Staged(direct_);
  options.cancelled = cancelled;
  options.progress = [cancelled](uint64_t, uint64_t) {
    cancelled->store(true);
    return true;
  };
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, std::move(options),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  ASSERT_FALSE(resumed);
  EXPECT_EQ(resumed.error().message, "Attachment send cancelled");
  EXPECT_TRUE(store_->HasCipher(staged_.id));
  EXPECT_FALSE(store_->ListForThread(direct_.id, TestDek()).empty());
  // Retry again, uncancelled: it sends.
  auto again_record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(again_record);
  Roe<ChatAttachmentFields> again = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *again_record, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { again = std::move(value); });
  EXPECT_TRUE(again);
}

// A Retry cancelled before it starts touches no network and keeps the staged copy.
TEST_F(StagedUploadTest, ACancelledResumeTouchesNoNetwork) {
  peer_.reachable = false;
  blob_.SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  blob_.ClearFileStartError();
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  const auto starts = blob_.FileStartCallCount();
  auto options = Staged(direct_);
  options.cancelled = std::make_shared<std::atomic<bool>>(true);
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, std::move(options),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  EXPECT_FALSE(resumed);
  EXPECT_EQ(blob_.FileStartCallCount(), starts);
  EXPECT_TRUE(store_->HasCipher(staged_.id));
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
