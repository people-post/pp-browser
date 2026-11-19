// "Send run logs" (DEV/PLAN-send-run-logs.md, step 1): the words a person types must not reach the log. Each case
// puts a unique sentinel into one input path that this target can drive (message text, a contact's name, a group's
// name, an attachment's file name), with every logger at the lowest level, and finds it in no line written.
// The paths this target cannot drive (the relay send itself, Amp / mesh) rest on the audit and the pre-filter.
//
// Run only these: --gtest_filter='RunLogSentinel*' (the rest of this binary opens the microphone and camera).
#include "common/Logger.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplaySources.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kSentinel = "zq-sentinel-9147-雪";

/**
 * Every line any logger writes, at the lowest level. Handlers cannot be taken off a logger, so there is one for the
 * process; each test starts it empty.
 */
class SentinelLogCapture : public logging::Handler {
public:
  static SentinelLogCapture& Start() {
    static const std::shared_ptr<SentinelLogCapture> capture = [] {
      auto made = std::make_shared<SentinelLogCapture>();
      logging::getRootLogger().addHandler(made);
      return made;
    }();
    logging::getRootLogger().setLevel(logging::Level::DEBUG);
    logging::setEmitFloor(logging::Level::DEBUG);
    capture->Clear();
    return *capture;
  }
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(message);
  }
  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
  }
  std::vector<std::string> LinesWith(const std::string& needle) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> found;
    for (const std::string& line : lines_) {
      if (line.find(needle) != std::string::npos) {
        found.push_back(line);
      }
    }
    return found;
  }

private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xc0 + i);
  }
  return dek;
}

class RunLogSentinelTest : public ::testing::Test {
protected:
  void SetUp() override {
    capture_ = &SentinelLogCapture::Start();
    // The capture is live at DEBUG: a line it must see.
    logging::getRootLogger().debug << "run-log-sentinel-control";
    ASSERT_EQ(capture_->LinesWith("run-log-sentinel-control").size(), 1u);

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_run_log_sentinel_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    shadows_ = std::make_unique<DirectoryShadowCache>(directory_);
    labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, PeerDisplaySourcesFrom(*shadows_));
    inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *labels_, shadows_.get());
    inbox_->SetProfileDataDir(data_dir_.string());
  }
  void TearDown() override {
    inbox_.reset();
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  Contact SaveContact(const std::string& id, const std::string& name, const std::string& nickname) {
    Contact contact;
    contact.id = id;
    contact.local.display_name = name;
    contact.remote.nickname = nickname;
    contact.remote.ids = {{mesh::ContactIdKind::Account, "account:" + id, true}};
    SyncContactMirrors(contact);
    auto saved = contacts_->Upsert(contact);
    EXPECT_TRUE(saved) << (saved ? "" : saved.error().message);
    return saved ? *saved : contact;
  }

  void ExpectNoSentinel() {
    for (const std::string& line : capture_->LinesWith(kSentinel)) {
      ADD_FAILURE() << "user text reached the log: " << line;
    }
  }

  SentinelLogCapture* capture_ = nullptr;
  std::filesystem::path data_dir_;
  MockDirectoryClient directory_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<DirectoryShadowCache> shadows_;
  std::unique_ptr<PeerDisplayResolver> labels_;
  std::unique_ptr<InboxController> inbox_;
};

TEST_F(RunLogSentinelTest, MessageTextIsNeverLogged) {
  const std::string body = std::string("my secret words ") + kSentinel + "\nsecond line " + kSentinel;
  SaveContact("c-1", "Ann", "ann");
  auto direct = inbox_->FindOrCreateDirectThread("c-1", ThreadChannel::E2ePublic);
  ASSERT_TRUE(direct) << direct.error().message;
  auto ai = inbox_->CreateAiThread();
  ASSERT_TRUE(ai) << ai.error().message;

  for (const std::string& thread_id : {direct->id, ai->id}) {
    ThreadMessage message;
    message.id = util::GenerateUuid();
    message.thread_id = thread_id;
    message.sender_contact_id = thread_id == direct->id ? "c-1" : "";
    message.text = body;
    message.timestamp = 1;
    ASSERT_TRUE(store_->AppendMessage(message));
    inbox_->OnInboundMessagePersisted(thread_id, body, /*attentive=*/false);
    ASSERT_TRUE(inbox_->UpdatePreview(thread_id, body));
    (void)inbox_->BuildTranscript(thread_id);
    inbox_->MarkThreadRead(thread_id);
  }
  (void)inbox_->NameAiThreadFromFirstMessage(ai->id, body);
  (void)inbox_->ListThreads();
  ExpectNoSentinel();
}

TEST_F(RunLogSentinelTest, AContactsNameIsNeverLogged) {
  const std::string name = std::string("Mary ") + kSentinel;
  SaveContact("c-2", name, std::string("nick ") + kSentinel);
  auto thread = inbox_->FindOrCreateDirectThread("c-2", ThreadChannel::E2ePublic);
  ASSERT_TRUE(thread) << thread.error().message;
  (void)inbox_->ResolveThreadLabel(*thread);
  (void)inbox_->ListThreads();
  (void)contacts_->SearchLocal(kSentinel);
  // A rename (the contact page's remark).
  auto renamed = contacts_->Get("c-2");
  ASSERT_TRUE(renamed && *renamed);
  Contact contact = **renamed;
  contact.local.display_name = std::string("Renamed ") + kSentinel;
  SyncContactMirrors(contact);
  ASSERT_TRUE(contacts_->Upsert(contact));
  (void)inbox_->ListThreads();
  ExpectNoSentinel();
}

TEST_F(RunLogSentinelTest, AGroupsNameIsNeverLogged) {
  SaveContact("c-3", "Bea", "bea");
  SaveContact("c-4", "Cai", "cai");
  // A group as the store holds it once made or joined (making one needs the group protocol, not driven here).
  Thread group;
  group.id = "g-1";
  group.kind = ThreadKind::Group;
  group.group_id = "group-1";
  group.title = std::string("Family ") + kSentinel;
  group.participant_contact_ids = {"c-3", "c-4"};
  group.updated_at = 1;
  ASSERT_TRUE(store_->UpsertThread(group));
  (void)inbox_->ResolveThreadLabel(group);
  ASSERT_TRUE(inbox_->SetThreadLocalTitle(group.id, std::string("Our own name ") + kSentinel));
  (void)inbox_->OpenThread(group.id);
  (void)inbox_->ListThreads();
  ExpectNoSentinel();
}

TEST_F(RunLogSentinelTest, AnAttachmentsFileNameIsNeverLogged) {
  SaveContact("c-5", "Dan", "dan");
  auto thread = inbox_->FindOrCreateDirectThread("c-5", ThreadChannel::E2ePublic);
  ASSERT_TRUE(thread) << thread.error().message;
  ChatAttachmentFields fields;
  fields.url = "https://cdn.example/file";
  fields.mime = "application/pdf";
  fields.filename = std::string("tax return ") + kSentinel + ".pdf";
  fields.byte_length = 1234;
  fields.content_hash.assign(32, 7);
  fields.content_key.assign(32, 1);
  fields.blob_nonce.assign(24, 2);
  ThreadMessage message;
  message.id = util::GenerateUuid();
  message.thread_id = thread->id;
  message.sender_contact_id = "c-5";
  message.content_type = ChatContentType::Attachment;
  message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  message.timestamp = 1;
  ASSERT_TRUE(store_->AppendMessage(message));
  inbox_->OnInboundMessagePersisted(thread->id, fields.filename, /*attentive=*/false);
  (void)inbox_->BuildTranscript(thread->id);
  (void)inbox_->Facts().attachment(message);
  (void)inbox_->ListThreads();
  ExpectNoSentinel();
}

} // namespace
} // namespace pbr
