// Attachments over the relay's files API (start → PUT → complete; a 302 download): the share link, the clean-up
// of a blob whose send failed after start, the server copy's delete, the log redaction of the link's token, and
// the one-GET-per-failure rule of the download budget.
#include "domain/attachments/AttachmentClient.h"
#include "domain/attachments/AttachmentFetch.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/RelayFileUrl.h"

#include "common/Logger.h"
#include "common/Utilities.h"
#include "common/chat/MessagingLimits.h"
#include "common/runtime/AppRuntime.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/error/AppError.h"
#include "lib/delivery/AttachmentContentCipher.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/messaging/AttachmentSuppressionStore.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/IdentityStore.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::ScriptedHttpServer;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0x50 + i);
  }
  return dek;
}

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

/** A sender with a registered identity, a group thread (server only) and the outgoing store. */
class AttachmentRelayUploadTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_relay_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    ASSERT_TRUE(identity_->LoadOrCreate());
    LocalIdentity updated = *identity_->Get();
    updated.relay_user_id = "relay:local";
    updated.registered = true;
    ASSERT_TRUE(identity_->Update(updated));
    store_ = std::make_unique<OutgoingAttachmentStore>(dir_.string(), "profile-1");
    group_.id = "thread-group";
    group_.kind = ThreadKind::Group;
    group_.channel = ThreadChannel::E2e;
    group_.group_id = "g1";
  }
  void TearDown() override {
    store_.reset();
    identity_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  ChatAttachmentUploadOptions Options(const bool staged) {
    ChatAttachmentUploadOptions options;
    options.thread = group_;
    options.thread_id = group_.id;
    if (staged) {
      options.outgoing = store_.get();
      options.dek = TestDek();
      options.on_staged = [this](const OutgoingAttachmentRecord& record) { staged_ = record; };
    }
    return options;
  }
  Roe<ChatAttachmentFields> Upload(const size_t size, const bool staged) {
    Roe<ChatAttachmentFields> result = Error("not called");
    UploadChatAttachmentAsync(blob_, *identity_, std::vector<uint8_t>(size, 0x42), "photo.jpg", "", Options(staged),
                              [&result](Roe<ChatAttachmentFields> value) { result = std::move(value); });
    return result;
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<OutgoingAttachmentStore> store_;
  Thread group_;
  MockBlobClient blob_;
  OutgoingAttachmentRecord staged_;
};

TEST_F(AttachmentRelayUploadTest, TheMessageCarriesTheRelayShareLink) {
  for (const bool staged : {false, true}) {
    auto fields = Upload(3000, staged);
    ASSERT_TRUE(fields) << fields.error().message;
    EXPECT_TRUE(std::regex_match(
        fields->url, std::regex(R"(https://mock\.example/api/relay/v1/files/[0-9a-f-]+/download\?t=[A-Za-z0-9_-]+)")))
        << fields->url;
    ASSERT_FALSE(blob_.CompletedFileIds().empty());
    EXPECT_EQ(RelayFileUrl::BlobIdOf(fields->url), blob_.CompletedFileIds().back());
    EXPECT_EQ(blob_.FileStartByteLengths().back(), 3000u + kAttachmentCipherOverheadBytes);
  }
  auto record = store_->Load(staged_.id, TestDek());
  ASSERT_TRUE(record);
  EXPECT_TRUE(record->delivered);
  EXPECT_EQ(RelayFileUrl::BlobIdOf(record->fields.url), blob_.CompletedFileIds().back()) << "the link is kept";
  EXPECT_TRUE(blob_.DeletedFileIds().empty());
}

TEST_F(AttachmentRelayUploadTest, AFailedPutDeletesTheStartedFileAndReportsThePutError) {
  for (const bool staged : {false, true}) {
    blob_.SetPutError(AppError::Blob(Err::Blob::Unavailable, "Blob PUT failed with status 503"));
    auto fields = Upload(3000, staged);
    ASSERT_FALSE(fields);
    EXPECT_EQ(fields.error().code, static_cast<int32_t>(Err::Blob::Unavailable));
  }
  EXPECT_EQ(blob_.DeletedFileIds().size(), 2u);
  EXPECT_TRUE(blob_.CompletedFileIds().empty());
  EXPECT_TRUE(store_->HasCipher(staged_.id)) << "kept for Retry";
}

TEST_F(AttachmentRelayUploadTest, ACompleteThatIsNoLongerPendingDeletesTheFile) {
  blob_.SetFileCompleteError(AppError::Blob(Err::Blob::Generic, "HTTP 400 BLOB_NOT_PENDING: Blob is not pending"));
  auto fields = Upload(3000, /*staged=*/true);
  ASSERT_FALSE(fields);
  EXPECT_EQ(RelayFileErrorCode(fields.error()), "BLOB_NOT_PENDING");
  ASSERT_EQ(blob_.DeletedFileIds().size(), 1u);
  EXPECT_EQ(blob_.DeletedFileIds().front(), "00000000-0000-4000-8000-000000000001");
}

// complete gives the link once: if it cannot be kept, the file is deleted and the send fails (a retry uploads
// again) rather than a message going out whose resend could never find its copy.
TEST_F(AttachmentRelayUploadTest, AStoreWriteFailureAfterCompleteDeletesTheFileAndFails) {
  blob_.SetOnFileComplete([this](const std::string&) {
    const std::string record_path = store_->CipherPath(staged_.id) + ".rec";
    std::filesystem::remove(record_path);
    std::filesystem::create_directory(record_path); // the record can no longer be written
  });
  auto fields = Upload(3000, /*staged=*/true);
  ASSERT_FALSE(fields);
  ASSERT_EQ(blob_.CompletedFileIds().size(), 1u);
  EXPECT_EQ(blob_.DeletedFileIds(), blob_.CompletedFileIds());
}

TEST(AttachmentUploadedCopyTest, DeletingTheServerCopyUsesTheLinksBlobId) {
  MockBlobClient blob;
  EXPECT_TRUE(DeleteUploadedAttachmentCopy(blob, "relay:u1", "")) << "sent peer-direct: nothing to delete";
  EXPECT_TRUE(blob.DeletedFileIds().empty());

  ASSERT_TRUE(DeleteUploadedAttachmentCopy(blob, "relay:u1",
                                           "https://www.brief.global/api/relay/v1/files/abc-123/download?t=tok"));
  EXPECT_EQ(blob.DeletedFileIds(), (std::vector<std::string>{"abc-123"}));
}

TEST(AttachmentUploadedCopyTest, ALinkThatIsNotARelayFileIsAnErrorNotASilentSuccess) {
  MockBlobClient blob;
  auto deleted = DeleteUploadedAttachmentCopy(blob, "relay:u1", "https://cdn.example/pp/files/abc-123?sig=1");
  ASSERT_FALSE(deleted);
  EXPECT_EQ(deleted.error().code, static_cast<int32_t>(Err::Blob::CopyNotDeleted));
  EXPECT_EQ(deleted.error().message.find("sig=1"), std::string::npos) << "the query is not repeated";
  EXPECT_TRUE(blob.DeletedFileIds().empty());
}

/** Every line any logger emits while `on`. */
class LineCapture : public logging::Handler {
public:
  void emit(logging::Level, const std::string&, const std::string& message) override {
    if (!on.load()) {
      return;
    }
    std::lock_guard lock(mutex_);
    lines_.push_back(message);
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mutex_);
    return std::exchange(lines_, {});
  }
  std::atomic<bool> on{false};

private:
  std::mutex mutex_;
  std::vector<std::string> lines_;
};

const std::regex& TokenPattern() {
  static const std::regex pattern(R"([?&]t=[A-Za-z0-9_-]{16,}|download\?t=)");
  return pattern;
}

bool AnyLineHasAToken(const std::vector<std::string>& lines) {
  for (const std::string& line : lines) {
    if (std::regex_search(line, TokenPattern())) {
      return true;
    }
  }
  return false;
}

bool AnyLineHas(const std::vector<std::string>& lines, const std::string& needle) {
  for (const std::string& line : lines) {
    if (line.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

class AttachmentLogRedactionTest : public AttachmentRelayUploadTest {
protected:
  void SetUp() override {
    AttachmentRelayUploadTest::SetUp();
    static const std::shared_ptr<LineCapture> shared = [] {
      auto made = std::make_shared<LineCapture>();
      logging::getRootLogger().addHandler(made);
      return made;
    }();
    capture_ = shared;
    previous_level_ = logging::getRootLogger().getLevel();
    logging::getRootLogger().setLevel(logging::kLevelDebug);
    capture_->Take();
    capture_->on = true;
  }
  void TearDown() override {
    capture_->on = false;
    logging::getRootLogger().setLevel(previous_level_);
    AttachmentRelayUploadTest::TearDown();
  }

  std::shared_ptr<LineCapture> capture_;
  logging::Level previous_level_ = logging::Level::INFO;
  ScriptedHttpServer server_;
};

TEST_F(AttachmentLogRedactionTest, NoLogLineCarriesTheShareTokenOrTheStorageUrl) {
  const std::string blob_id = "0b8c2a52-6f2e-4a52-9d1e-1f2a3b4c5d6e";
  const std::string link =
      server_.Origin() + "/api/relay/v1/files/" + blob_id + "/download?t=Zx9_Token-abcdefghijklmnopqrstuvwxyz0123";

  // The pattern catches a whole link: a line that prints one is caught.
  logging::getLogger("RelayFileLogProbe").info << "deliberately unredacted " << link;
  ASSERT_TRUE(AnyLineHasAToken(capture_->Take())) << "the check below would prove nothing";

  // Send (start, complete; a failed PUT abandons) and receive (302 to storage; a 404), as the app does.
  ASSERT_TRUE(Upload(3000, /*staged=*/true));
  blob_.SetPutError(AppError::Blob(Err::Blob::Unavailable, "Blob PUT failed with status 503"));
  ASSERT_FALSE(Upload(3000, /*staged=*/false));

  auto prepared = PrepareChatAttachmentFromBytes(std::vector<uint8_t>(5000, 0x33), "a.bin", "", 1'000'000);
  ASSERT_TRUE(prepared);
  const std::string cipher(prepared->ciphertext.begin(), prepared->ciphertext.end());
  const std::string storage = server_.Origin() + "/s3/object?X-Amz-Signature=deadbeefdeadbeefdeadbeef";
  server_.SetHandler([&](const ScriptedHttpServer::Request& request) {
    if (request.target.rfind("/api/relay/v1/files/" + blob_id + "/download?t=", 0) == 0) {
      return ScriptedHttpServer::Reply{.status = 302, .headers = {{"Location", storage}}, .body = ""};
    }
    if (request.target.rfind("/s3/object", 0) == 0) {
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = cipher};
    }
    return ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = R"({"error":"Not found"})"};
  });
  ChatAttachmentFields fields = prepared->fields;
  fields.url = link;
  AttachmentFetchContext context;
  context.allow_local_cdn_for_test = true;
  context.profile_data_dir = dir_.string(); // streamed to a file
  auto streamed = FetchAndDecryptAttachment(fields, context);
  ASSERT_TRUE(streamed) << streamed.error().message;
  EXPECT_EQ(streamed->size(), 5000u);
  context.profile_data_dir.clear(); // buffered
  ASSERT_TRUE(FetchAndDecryptAttachment(fields, context));

  fields.url = server_.Origin() + "/api/relay/v1/files/" + blob_id + "x/download?t=Gone_Token-abcdefghijklmnop";
  auto gone = FetchAttachmentCiphertext(fields, context);
  ASSERT_FALSE(gone);
  EXPECT_EQ(gone.error().code, static_cast<int32_t>(Err::Blob::Expired));

  const auto lines = capture_->Take();
  EXPECT_FALSE(AnyLineHasAToken(lines));
  EXPECT_FALSE(AnyLineHas(lines, "X-Amz-Signature"));
  EXPECT_FALSE(AnyLineHas(lines, "/s3/"));
  // ...while the lines the device check reads are there.
  EXPECT_TRUE(AnyLineHas(lines, "relay file start blob=")) << ::testing::PrintToString(lines);
  EXPECT_TRUE(AnyLineHas(lines, "mode=single"));
  EXPECT_TRUE(AnyLineHas(lines, "relay file complete blob="));
  EXPECT_TRUE(AnyLineHas(lines, "relay file abandon blob="));
  EXPECT_TRUE(AnyLineHas(lines, "step=put"));
  EXPECT_TRUE(AnyLineHas(lines, "attachment fetch blob=" + blob_id + " http=200 redirects=1"));
  EXPECT_TRUE(AnyLineHas(lines, "http=404 redirects=0"));
}

// Every server GET spends one of the file's ten downloads, and the relay answers expired, used up and deleted with
// the same 404: after a failure only the person's tap asks again.
class AttachmentDownloadSuppressionTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_attachment_suppress_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    suppression_ = std::make_unique<AttachmentSuppressionStore>(dir_.string());
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetProfileId("test");
    workflow_.SetAllowLocalCdnForTest(true);
    workflow_.SetSuppressionStore(suppression_.get());
    ASSERT_TRUE(workflow_.SetDek(TestDek()));
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
    ASSERT_TRUE(threads_.SetDek(TestDek()));
    Thread thread;
    thread.id = "t1";
    thread.kind = ThreadKind::Group;
    thread.group_id = "g1";
    thread.title = "Group";
    ASSERT_TRUE(threads_.UpsertThread(thread));
    server_.SetHandler([](const ScriptedHttpServer::Request&) {
      return ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = R"({"error":"Not found"})"};
    });
  }
  void TearDown() override {
    AppRuntime::Shutdown();
    suppression_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  size_t Gets() const { return server_.Requests().size(); }

  std::filesystem::path dir_;
  std::unique_ptr<AttachmentSuppressionStore> suppression_;
  InMemoryDeliveryStore threads_;
  AttachmentFetchWorkflow workflow_;
  ScriptedHttpServer server_;
};

TEST_F(AttachmentDownloadSuppressionTest, AFailedDownloadIsNotFetchedAgainUntilTheRetryTap) {
  auto prepared = PrepareChatAttachmentFromBytes(std::vector<uint8_t>(2000, 0x21), "p.jpg", "", 1'000'000);
  ASSERT_TRUE(prepared);
  ChatAttachmentFields fields = prepared->fields;
  fields.url = server_.Origin() + "/api/relay/v1/files/f-1/download?t=Expired_Token-abcdefghijk";
  ThreadMessage message;
  message.id = "m1";
  message.thread_id = "t1";
  message.sender_contact_id = "alice";
  message.content_type = ChatContentType::Attachment;
  message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  message.timestamp = 1;
  ASSERT_TRUE(threads_.AppendMessage(message));
  const auto state = [&] { return workflow_.StateFor("t1", fields.content_hash, fields.byte_length); };

  workflow_.EnqueueFromMessage("t1", message, /*force=*/true);
  ASSERT_TRUE(WaitFor([&] { return state() == AttachmentFetchWorkflow::DownloadState::Failed; }));
  EXPECT_EQ(Gets(), 1u);
  EXPECT_TRUE(suppression_->IsSuppressed("t1", fields.content_hash));

  // What opening the chat and a restart's backlog do: nothing goes out.
  workflow_.EnqueueFromMessage("t1", message);
  workflow_.EnsureThreadQueued("t1", threads_);
  workflow_.DrainPendingMediaBacklog(threads_);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(Gets(), 1u) << "no automatic GET after a failure";

  // The card's Download / Retry: one more GET.
  workflow_.RetryDownload("t1", "m1", threads_);
  ASSERT_TRUE(WaitFor([&] { return Gets() == 2u; }));
  ASSERT_TRUE(WaitFor([&] { return state() == AttachmentFetchWorkflow::DownloadState::Failed; }));
  EXPECT_TRUE(suppression_->IsSuppressed("t1", fields.content_hash)) << "failed again: held again";
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_EQ(Gets(), 2u);
}

} // namespace
} // namespace pbr
