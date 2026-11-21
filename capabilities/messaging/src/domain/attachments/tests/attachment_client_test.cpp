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
#include <memory>
#include <optional>

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
  /** Set: the push does not answer until the test calls `answer` (a dial that takes its time). */
  bool hold = false;
  std::function<void(Roe<void>)> answer;

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
  void PushChatBlobAsync(const ChatBlobRequest& request, const std::vector<uint8_t>& ciphertext,
                         std::function<void(Roe<void>)> on_done) override {
    if (!hold) {
      IChatBlobPeerClient::PushChatBlobAsync(request, ciphertext, std::move(on_done));
      return;
    }
    ++push_calls;
    answer = std::move(on_done);
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
    updated.registered = true;
    ASSERT_TRUE(identity_->Update(updated));

    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    Contact peer;
    peer.id = "contact-peer";
    peer.remote.ids.push_back(mesh::ContactId{.kind = mesh::ContactIdKind::Account, .value = "account:peer"});
    peer.local.display_name = "Peer";
    SyncContactMirrors(peer);
    ASSERT_TRUE(contacts_->Upsert(peer));

    direct_.id = "thread-direct";
    direct_.kind = ThreadKind::Direct;
    direct_.channel = ThreadChannel::E2e;
    direct_.participant_contact_ids = {"contact-peer"};
    direct_.peer_identity_kind = "account";
    direct_.peer_identity_value = "account:peer";

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
  std::shared_ptr<MockBlobClient> blob_ = std::make_shared<MockBlobClient>();
};

TEST_F(AttachmentClientTest, SmallFileToAReachableContactGoesDirectAndNotToTheServer) {
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
  EXPECT_TRUE(fields->url.empty()) << "a direct push leaves no server copy";
  EXPECT_EQ(fields->mime, "application/pdf");
}

TEST_F(AttachmentClientTest, DirectFailureFallsBackToTheServer) {
  peer_.push_ok = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1) << "direct was tried first";
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
  ASSERT_EQ(blob_->UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_->CompletedFileIds().size(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, TheSendKeepsTheClientItStartedWithWhenTheCallersIsReplaced) {
  // The preferred relay changes while a send waits on the direct push (MR006): the caller's client is rebuilt and
  // its old one dropped. The send finishes on the client it started with, which it holds until then.
  peer_.push_ok = false;
  std::weak_ptr<MockBlobClient> started = blob_;
  bool held_after_drop = false;
  peer_.on_push = [this, &started, &held_after_drop]() {
    blob_.reset();
    held_after_drop = !started.expired();
  };
  Roe<ChatAttachmentFields> result = Error("not called");
  size_t uploaded = 0;
  UploadChatAttachmentAsync(blob_, *identity_, std::vector<uint8_t>(1000, 0x42), "doc.pdf", "", Options(direct_),
                            [&result, &uploaded, &started](Roe<ChatAttachmentFields> value) {
                              if (auto client = started.lock()) {
                                uploaded = client->UploadedBodies().size();
                              }
                              result = std::move(value);
                            });
  EXPECT_TRUE(held_after_drop) << "the send holds its client";
  ASSERT_TRUE(result) << result.error().message;
  EXPECT_EQ(uploaded, 1u) << "the fallback upload went through the client the send started with";
  EXPECT_TRUE(started.expired()) << "and lets it go once done";
}

TEST_F(AttachmentClientTest, UnreachableContactGoesStraightToTheServer) {
  peer_.reachable = false;
  auto fields = Upload(1000, Options(direct_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, GroupAttachmentsAreServerOnly) {
  auto fields = Upload(1000, Options(group_));
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "no peer push in a group, even with a reachable member";
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, AboveTheDirectLimitSkipsDirectAndGoesToTheServer) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL; // a raised limit, as step 2 will ship
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes) + 1, options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 0) << "bigger than kMaxDirectAttachmentBytes never tries the 15 s direct path";
  ASSERT_EQ(blob_->FileStartCallCount(), 1u);
  EXPECT_EQ(blob_->FileStartByteLengths().front(), kMaxDirectAttachmentBytes + 1 + kAttachmentCipherOverheadBytes)
      << "the relay reserves the ciphertext length";
  EXPECT_FALSE(fields->url.empty());
}

TEST_F(AttachmentClientTest, ExactlyTheDirectLimitStillGoesDirect) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL;
  auto fields = Upload(static_cast<size_t>(kMaxDirectAttachmentBytes), options);
  ASSERT_TRUE(fields) << fields.error().message;
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
}

TEST_F(AttachmentClientTest, OverTheCallersLimitFailsBeforeAnyNetwork) {
  auto options = Options(direct_);
  options.max_plaintext_bytes = 999;
  auto fields = Upload(1000, options);
  EXPECT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
}

TEST_F(AttachmentClientTest, ServerErrorsKeepTheirTypeToTheCaller) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::QuotaExceeded,
                                          "HTTP 429 QUOTA_FILE_BYTES: File storage quota exceeded byte_length=1016"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::QuotaExceeded));
}

TEST_F(AttachmentClientTest, NotConfiguredServerAfterAFailedDirectPushSaysBoth) {
  peer_.reachable = false; // no live direct link: the push fails before it is sent
  blob_->SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
}

TEST_F(AttachmentClientTest, NotConfiguredServerAfterARefusedDirectPushSaysBoth) {
  peer_.push_ok = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(direct_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 1);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
}

TEST_F(AttachmentClientTest, NotConfiguredServerInAGroupKeepsTheServerOnlyMessage) {
  blob_->SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  auto fields = Upload(1000, Options(group_));
  ASSERT_FALSE(fields);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfigured));
}

TEST_F(AttachmentClientTest, OtherServerErrorsAfterAFailedDirectPushKeepTheirType) {
  peer_.push_ok = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
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
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
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
  EXPECT_EQ(blob_->DeletedFileIds().size(), blob_->UploadedBodies().size()) << "whatever went up is deleted";
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
  ASSERT_EQ(blob_->UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_->UploadedBodies().front(), std::string(cipher->begin(), cipher->end()))
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
  ASSERT_EQ(blob_->UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_->UploadedBodies().front(), std::string(cipher->begin(), cipher->end()));
}

TEST_F(StagedUploadTest, AFailedUploadKeepsTheCiphertextAndAResumeSendsTheSameBytesWithTheSameKey) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  auto failed = UploadStaged(5000, Staged(direct_));
  ASSERT_FALSE(failed);
  ASSERT_FALSE(staged_.id.empty());
  EXPECT_TRUE(store_->HasCipher(staged_.id)) << "kept for Retry";
  const auto cipher_before = *store_->ReadCipher(staged_.id);
  const auto key_before = staged_.fields.content_key;
  auto sealed = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(sealed);
  EXPECT_FALSE(sealed->delivered);

  blob_->ClearFileStartError();
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *sealed, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  ASSERT_TRUE(resumed) << resumed.error().message;
  EXPECT_EQ(resumed->content_key, key_before) << "same key: the message the recipient gets still decrypts";
  ASSERT_EQ(blob_->UploadedBodies().size(), 1u);
  EXPECT_EQ(blob_->UploadedBodies().front(), std::string(cipher_before.begin(), cipher_before.end()));
  EXPECT_FALSE(resumed->url.empty());
}

TEST_F(StagedUploadTest, NotConfiguredServerAfterAFailedDirectPushSaysBothAndAGroupDoesNot) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
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
  const auto starts = blob_->FileStartCallCount();
  Roe<ChatAttachmentFields> again = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, Staged(direct_),
                            [&](Roe<ChatAttachmentFields> value) { again = std::move(value); });
  ASSERT_TRUE(again);
  EXPECT_EQ(again->url, fields->url);
  EXPECT_EQ(blob_->FileStartCallCount(), starts);
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
  ASSERT_EQ(blob_->FileStartCallCount(), 1u);
  EXPECT_EQ(blob_->FileStartByteLengths().front(), staged_.cipher_bytes) << "ciphertext length, not the file's";
}

TEST_F(StagedUploadTest, ResumeWithoutTheCiphertextTellsTheCallerToPickAgain) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  std::filesystem::remove(store_->CipherPath(staged_.id)); // cleaned after a day, or lost
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
  EXPECT_EQ(blob_->DeletedFileIds().size(), blob_->UploadedBodies().size()) << "whatever went up is deleted";
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
  EXPECT_TRUE(blob_->UploadedBodies().empty());
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
  EXPECT_TRUE(blob_->DeletedFileIds().empty());
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
}

// A cancel during a staged Retry only stops that attempt: the record stays, so Retry still works.
TEST_F(StagedUploadTest, ACancelledResumeKeepsTheRecordForRetry) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  blob_->ClearFileStartError();
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
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  ASSERT_FALSE(UploadStaged(5000, Staged(direct_)));
  blob_->ClearFileStartError();
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  const auto starts = blob_->FileStartCallCount();
  auto options = Staged(direct_);
  options.cancelled = std::make_shared<std::atomic<bool>>(true);
  Roe<ChatAttachmentFields> resumed = Error("not called");
  ResumeChatAttachmentAsync(blob_, *identity_, *record, std::move(options),
                            [&](Roe<ChatAttachmentFields> value) { resumed = std::move(value); });
  EXPECT_FALSE(resumed);
  EXPECT_EQ(blob_->FileStartCallCount(), starts);
  EXPECT_TRUE(store_->HasCipher(staged_.id));
}

// The head start (kDirectHeadStart): a direct push that has not answered by then gets the server upload beside it,
// and the first to deliver gives the one result. The test holds the push's answer and fires the head start itself.
class DirectHeadStartTest : public StagedUploadTest {
protected:
  ChatAttachmentUploadOptions Raced(const Thread& thread) {
    auto options = Staged(thread);
    options.run_after = [this](const std::chrono::milliseconds delay, std::function<void()> task) {
      head_start_delay_ = delay;
      ++head_starts_;
      head_start_ = std::move(task);
    };
    return options;
  }
  void Send(ChatAttachmentUploadOptions options, const size_t size = 5000) {
    const std::vector<uint8_t> bytes(size, 0x42);
    UploadChatAttachmentAsync(blob_, *identity_, bytes, "voice.ogg", "", std::move(options),
                              [this](Roe<ChatAttachmentFields> value) {
                                ++results_;
                                result_ = std::move(value);
                              });
  }
  void FireHeadStart() {
    ASSERT_TRUE(head_start_);
    auto task = std::move(head_start_);
    head_start_ = nullptr;
    task();
  }
  std::optional<OutgoingAttachmentRecord> Record() {
    auto loaded = store_->Load(staged_.id, TestDek());
    if (!loaded) {
      return std::nullopt;
    }
    return *loaded;
  }
  std::function<void()> head_start_;
  std::chrono::milliseconds head_start_delay_{0};
  int head_starts_ = 0;
  int results_ = 0;
  Roe<ChatAttachmentFields> result_ = Error("not called");
};

TEST_F(DirectHeadStartTest, ASlowPushGetsTheServerBesideItAfterOneSecondAndTheServerResultIsSent) {
  peer_.hold = true;
  Send(Raced(direct_));
  EXPECT_EQ(head_start_delay_, kDirectHeadStart);
  EXPECT_EQ(results_, 0) << "nothing yet: the push is out";
  EXPECT_EQ(blob_->FileStartCallCount(), 0u) << "the push runs alone for its head start";
  FireHeadStart();
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_) << result_.error().message;
  EXPECT_FALSE(result_->url.empty()) << "the server delivered it";
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
  auto record = Record();
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
  EXPECT_EQ(record->fields.url, result_->url);
}

TEST_F(DirectHeadStartTest, APushThatSucceedsAfterTheServerWonChangesNothing) {
  peer_.hold = true;
  Send(Raced(direct_));
  FireHeadStart();
  ASSERT_EQ(results_, 1);
  const std::string url = result_->url;
  ASSERT_TRUE(peer_.answer);
  peer_.answer(Roe<void>{}); // the contact got the bytes after all, 10 s later
  EXPECT_EQ(results_, 1) << "one result, so one message";
  ASSERT_TRUE(result_);
  EXPECT_EQ(result_->url, url);
  EXPECT_TRUE(blob_->DeletedFileIds().empty()) << "the server copy the message points at stays";
  auto record = Record();
  ASSERT_TRUE(record);
  EXPECT_EQ(record->fields.url, url) << "the late push does not rewrite the record";
  EXPECT_TRUE(store_->HasCipher(staged_.id)) << "the staged copy is the caller's to remove, as on any send";
}

TEST_F(DirectHeadStartTest, APushThatFailsAfterTheServerWonIsIgnored) {
  peer_.hold = true;
  Send(Raced(direct_));
  FireHeadStart();
  ASSERT_EQ(results_, 1);
  peer_.answer(Error("handshake timeout"));
  EXPECT_EQ(results_, 1);
  EXPECT_TRUE(result_) << "no error after the send succeeded";
}

TEST_F(DirectHeadStartTest, APushThatWinsWhileTheServerUploadsDeletesTheServerCopy) {
  peer_.hold = true;
  auto options = Raced(direct_);
  options.progress = [this](uint64_t, uint64_t) {
    if (peer_.answer) {
      auto answer = std::move(peer_.answer);
      peer_.answer = nullptr;
      answer(Roe<void>{}); // the push lands while the PUT is going
    }
    return true;
  };
  Send(std::move(options));
  FireHeadStart();
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_) << result_.error().message;
  EXPECT_TRUE(result_->url.empty()) << "the push delivered it";
  EXPECT_EQ(blob_->DeletedFileIds().size(), blob_->UploadedBodies().size()) << "the server copy is not left behind";
  auto record = Record();
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
  EXPECT_TRUE(record->fields.url.empty()) << "the losing upload does not write the record";
}

TEST_F(DirectHeadStartTest, AServerFailureWaitsForThePushStillOut) {
  peer_.hold = true;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  Send(Raced(direct_));
  FireHeadStart();
  EXPECT_EQ(results_, 0) << "the push may still deliver it";
  peer_.answer(Roe<void>{});
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_) << result_.error().message;
  EXPECT_TRUE(result_->url.empty());
}

TEST_F(DirectHeadStartTest, BothFailingGivesOneErrorThatSaysBoth) {
  peer_.hold = true;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::UploadNotConfigured, "HTTP 503: Blob upload is not configured"));
  Send(Raced(direct_));
  FireHeadStart();
  peer_.answer(Error("handshake timeout"));
  ASSERT_EQ(results_, 1);
  ASSERT_FALSE(result_);
  EXPECT_EQ(result_.error().code, static_cast<int32_t>(Err::Blob::UploadNotConfiguredNoDirect));
  EXPECT_TRUE(store_->HasCipher(staged_.id)) << "kept for Retry";
}

TEST_F(DirectHeadStartTest, APushAnsweredWithinTheHeadStartNeverStartsTheServer) {
  peer_.hold = true;
  Send(Raced(direct_));
  peer_.answer(Roe<void>{});
  ASSERT_EQ(results_, 1);
  EXPECT_TRUE(result_->url.empty());
  FireHeadStart(); // the timer fires later anyway
  EXPECT_EQ(results_, 1);
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
}

TEST_F(DirectHeadStartTest, APushFailingWithinTheHeadStartStartsTheServerOnce) {
  peer_.hold = true;
  Send(Raced(direct_));
  peer_.answer(Error("not registered"));
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_);
  EXPECT_FALSE(result_->url.empty());
  FireHeadStart();
  EXPECT_EQ(results_, 1);
  EXPECT_EQ(blob_->FileStartCallCount(), 1u) << "the timer does not upload a second copy";
}

TEST_F(DirectHeadStartTest, AnUnreachableContactGetsNoHeadStart) {
  peer_.reachable = false;
  Send(Raced(direct_));
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_);
  EXPECT_EQ(head_starts_, 0) << "the push failed before it was sent: the server went at once";
}

TEST_F(DirectHeadStartTest, ACancelDuringTheRaceSendsNothingAndLeavesNothingBehind) {
  peer_.hold = true;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto options = Raced(direct_);
  options.cancelled = cancelled;
  options.progress = [cancelled](uint64_t, uint64_t) {
    cancelled->store(true); // ✕ while both legs are out
    return true;
  };
  Send(std::move(options));
  FireHeadStart();
  peer_.answer(Error("handshake timeout"));
  ASSERT_EQ(results_, 1) << "one result, whichever leg answered last";
  ASSERT_FALSE(result_);
  EXPECT_EQ(result_.error().message, "Attachment send cancelled");
  EXPECT_EQ(blob_->DeletedFileIds().size(), blob_->UploadedBodies().size()) << "whatever went up is deleted";
  EXPECT_TRUE(store_->ListForThread(direct_.id, TestDek()).empty());
}

TEST_F(DirectHeadStartTest, ACancelBeforeTheHeadStartKeepsTheServerOut) {
  peer_.hold = true;
  auto cancelled = std::make_shared<std::atomic<bool>>(false);
  auto options = Raced(direct_);
  options.cancelled = cancelled;
  Send(std::move(options));
  cancelled->store(true);
  FireHeadStart();
  EXPECT_EQ(blob_->FileStartCallCount(), 0u);
  peer_.answer(Error("handshake timeout"));
  ASSERT_EQ(results_, 1);
  EXPECT_FALSE(result_);
  EXPECT_TRUE(blob_->UploadedBodies().empty());
  EXPECT_TRUE(store_->ListForThread(direct_.id, TestDek()).empty());
}

TEST_F(DirectHeadStartTest, ARetryRacesTheSameWay) {
  peer_.reachable = false;
  blob_->SetFileStartError(AppError::Blob(Err::Blob::Unavailable, "HTTP 503"));
  Send(Raced(direct_));
  ASSERT_FALSE(result_);
  blob_->ClearFileStartError();
  peer_.reachable = true;
  peer_.hold = true;
  auto record = Record();
  ASSERT_TRUE(record);
  Roe<ChatAttachmentFields> resumed = Error("not called");
  int resumed_results = 0;
  ResumeChatAttachmentAsync(blob_, *identity_, *record, Raced(direct_), [&](Roe<ChatAttachmentFields> value) {
    ++resumed_results;
    resumed = std::move(value);
  });
  EXPECT_EQ(resumed_results, 0);
  FireHeadStart();
  ASSERT_EQ(resumed_results, 1);
  ASSERT_TRUE(resumed) << resumed.error().message;
  EXPECT_FALSE(resumed->url.empty());
  peer_.answer(Roe<void>{});
  EXPECT_EQ(resumed_results, 1);
}

TEST_F(DirectHeadStartTest, ALargeFileIsUnchangedServerOnlyWithNoHeadStart) {
  auto options = Raced(direct_);
  options.max_plaintext_bytes = 20ULL * 1000ULL * 1000ULL;
  Send(std::move(options), static_cast<size_t>(kMaxDirectAttachmentBytes) + 1);
  ASSERT_EQ(results_, 1);
  ASSERT_TRUE(result_);
  EXPECT_EQ(peer_.push_calls, 0);
  EXPECT_EQ(head_starts_, 0);
  EXPECT_EQ(blob_->FileStartCallCount(), 1u);
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
