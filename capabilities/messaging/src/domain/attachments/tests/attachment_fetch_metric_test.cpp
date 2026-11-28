// B248 F: one `attachment.fetch` metric per finished download: where the bytes came from, how long, how big, how it
// ended, how many waited behind it, and whether the account's other device sent it. Nothing that names anyone or
// anything (no ids, names, hashes, links).
#include "lib/content/testing/InMemoryObjectIndex.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"

#include "common/Metrics.h"
#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "common/thread/ThreadRecordTypes.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/sockets/testing/ScriptedHttpServer.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/ChatAttachmentPrepare.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace pbr {
namespace {

using State = AttachmentFetchWorkflow::DownloadState;
using pbr::test::ScriptedHttpServer;

bool WaitFor(const std::function<bool()>& done, const std::chrono::milliseconds budget = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

struct LineCapture : logging::Handler {
  void emit(logging::Level, const std::string&, const std::string& message) override {
    if (message.find("event=attachment.fetch ") == std::string::npos) {
      return;
    }
    std::lock_guard lock(mutex);
    lines.push_back(message);
  }
  std::vector<std::string> Take() {
    std::lock_guard lock(mutex);
    return std::exchange(lines, {});
  }
  size_t Count() {
    std::lock_guard lock(mutex);
    return lines.size();
  }
  std::mutex mutex;
  std::vector<std::string> lines;
};

/** The Metrics channel keeps its handlers: one capture for the whole binary. */
LineCapture& FetchMetrics() {
  static const std::shared_ptr<LineCapture> capture = [] {
    auto made = std::make_shared<LineCapture>();
    MetricsLog().addHandler(made);
    return made;
  }();
  return *capture;
}

/** The value of ` key=` in a metric line ("" when absent). */
std::string Field(const std::string& line, const std::string& key) {
  const std::string tag = " " + key + "=";
  const size_t at = line.find(tag);
  if (at == std::string::npos) {
    return "";
  }
  const size_t start = at + tag.size();
  return line.substr(start, line.find(' ', start) - start);
}

class FetchMetricTest : public ::testing::Test {
protected:
  void SetUp() override {
    (void)FetchMetrics().Take();
    AppRuntime::Initialize();
    dir_ = std::filesystem::temp_directory_path() / ("pp_fetch_metric_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    workflow_.SetProfileDataDir(dir_.string());
    workflow_.SetContent(MakeInMemoryIndexedCasStore(dir_.string(), "test"));
    workflow_.SetAllowLocalCdnForTest(true);
    ASSERT_TRUE(workflow_.SetDek(ByteVector(kDataEncryptionKeySize, 0x41)));
    workflow_.SetDiskUsageProvider([] { return uint64_t{0}; });
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mutex_);
      const auto found = bodies_.find(request.target);
      if (found == bodies_.end()) {
        return ScriptedHttpServer::Reply{.status = 404, .headers = {}, .body = "gone"};
      }
      return ScriptedHttpServer::Reply{.status = 200, .headers = {}, .body = found->second};
    });
  }
  void TearDown() override {
    AppRuntime::ResumeWorkers();
    AppRuntime::Shutdown();
    for (int i = 0; i < 100 && AppRuntime::HasPendingUITasks(); ++i) {
      AppRuntime::RunUITasks();
    }
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  enum class Where { Pushed, Server, Gone };
  /** A 2000-byte file in t1 from `sender`: already pushed to this device, served at `/name`, or gone (404). */
  ThreadMessage File(const std::string& name, const std::string& sender, const Where where,
                     ChatAttachmentFields* fields_out) {
    std::vector<uint8_t> plain(2'000, 0x21);
    for (size_t i = 0; i < name.size(); ++i) {
      plain[i] = static_cast<uint8_t>(name[i]);
    }
    auto prepared = PrepareChatAttachmentFromBytes(plain, name + ".pdf", "", 1'000'000);
    EXPECT_TRUE(prepared);
    ChatAttachmentFields fields = prepared->fields;
    if (where == Where::Pushed) {
      EXPECT_TRUE(SavePendingAttachmentCiphertext(dir_.string(), "t1", fields.content_hash, prepared->ciphertext, {}));
    } else {
      fields.url = server_.Origin() + "/" + name;
      if (where == Where::Server) {
        std::lock_guard lock(mutex_);
        bodies_["/" + name] = std::string(prepared->ciphertext.begin(), prepared->ciphertext.end());
      }
    }
    *fields_out = fields;
    ThreadMessage message;
    message.id = name;
    message.thread_id = "t1";
    message.sender_contact_id = sender;
    message.content_type = ChatContentType::Attachment;
    message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
    return message;
  }
  /** Nothing that names the file, the chat, the sender or where it lives. */
  static void ExpectNothingIdentifying(const std::string& line, const ChatAttachmentFields& fields,
                                       const std::string& sender) {
    EXPECT_EQ(line.find(fields.filename), std::string::npos) << line;
    EXPECT_EQ(line.find(AttachmentHashHex(fields.content_hash).substr(0, 8)), std::string::npos) << line;
    EXPECT_EQ(line.find("t1"), std::string::npos) << line;
    EXPECT_EQ(line.find(sender), std::string::npos) << line;
    EXPECT_EQ(line.find("127.0.0.1"), std::string::npos) << line;
    EXPECT_EQ(line.find("http"), std::string::npos) << line;
  }

  std::mutex mutex_;
  std::map<std::string, std::string> bodies_;
  ScriptedHttpServer server_; // after what its handler reads: stopped before those go
  std::filesystem::path dir_;
  AttachmentFetchWorkflow workflow_;
};

TEST_F(FetchMetricTest, OneLinePerDownloadWithHowManyWaitedBehindIt) {
  ChatAttachmentFields a;
  ChatAttachmentFields b;
  ChatAttachmentFields c;
  AppRuntime::PauseWorkers(); // all three queued before the first starts
  workflow_.EnqueueFromMessage("t1", File("a", kLocalSelfContactId, Where::Pushed, &a));
  workflow_.EnqueueFromMessage("t1", File("b", kLocalSelfContactId, Where::Pushed, &b));
  workflow_.EnqueueFromMessage("t1", File("c", kLocalSelfContactId, Where::Pushed, &c));
  AppRuntime::ResumeWorkers();
  ASSERT_TRUE(WaitFor([&] { return FetchMetrics().Count() == 3u; }));
  const std::vector<std::string> lines = FetchMetrics().Take();
  ASSERT_EQ(lines.size(), 3u);
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    EXPECT_EQ(Field(line, "source"), "pushed") << line;
    EXPECT_EQ(Field(line, "result"), "ok") << line;
    EXPECT_EQ(Field(line, "bytes"), "2000") << line;
    EXPECT_EQ(Field(line, "own"), "1") << line;
    EXPECT_EQ(Field(line, "queued"), std::to_string(2 - i)) << line;
    EXPECT_FALSE(Field(line, "ms").empty()) << line;
    ExpectNothingIdentifying(line, i == 0 ? a : (i == 1 ? b : c), kLocalSelfContactId);
  }
  EXPECT_EQ(workflow_.StateFor("t1", c), State::Ready);
}

TEST_F(FetchMetricTest, AServerDownloadFromSomeoneElseAndOneThatFailed) {
  ChatAttachmentFields served;
  workflow_.EnqueueFromMessage("t1", File("served", "alice", Where::Server, &served));
  ASSERT_TRUE(WaitFor([&] { return FetchMetrics().Count() == 1u; }));
  std::string line = FetchMetrics().Take().at(0);
  EXPECT_EQ(Field(line, "source"), "server") << line;
  EXPECT_EQ(Field(line, "result"), "ok") << line;
  EXPECT_EQ(Field(line, "own"), "0") << line;
  EXPECT_EQ(Field(line, "queued"), "0") << line;
  ExpectNothingIdentifying(line, served, "alice");

  ChatAttachmentFields gone;
  workflow_.EnqueueFromMessage("t1", File("gone", "alice", Where::Gone, &gone));
  ASSERT_TRUE(WaitFor([&] { return FetchMetrics().Count() == 1u; }));
  line = FetchMetrics().Take().at(0);
  EXPECT_EQ(Field(line, "source"), "server") << line;
  EXPECT_EQ(Field(line, "result"), "failed") << line;
  EXPECT_EQ(workflow_.StateFor("t1", gone), State::Failed);
}

} // namespace
} // namespace pbr
