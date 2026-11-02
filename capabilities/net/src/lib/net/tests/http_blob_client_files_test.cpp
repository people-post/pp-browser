#include "common/ValueJson.h"
#include "common/tests/ScriptedHttpServer.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"
#include "lib/net/BlobClient.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/net/RelayBlobSignPayload.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::ScriptedHttpServer;
using Reply = ScriptedHttpServer::Reply;

/** A relay `/files` stand-in on loopback: scripted answers per step, and the bodies it was sent. */
class FakeFilesRelay {
public:
  explicit FakeFilesRelay(ScriptedHttpServer& server) : server_(server) {
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) { return Answer(request); });
  }

  std::string BaseUrl() const { return server_.Origin() + "/api/relay"; }

  // Scripted answers (status 200 = the normal success body).
  Reply start_reply{.status = 0, .headers = {}, .body = ""}; // status 0: success with `start_mode`
  std::vector<Reply> start_replies;                          // consumed first, one per start
  std::string start_mode = "single";
  int put_status = 200;
  Reply complete_reply{.status = 0, .headers = {}, .body = ""}; // status 0: success
  std::string list_body = R"({"files":[],"usage":{}})";

  std::vector<Object> Bodies(const std::string& op) const {
    std::lock_guard lock(mutex_);
    std::vector<Object> out;
    for (const auto& [name, body] : bodies_) {
      if (name == op) {
        out.push_back(body);
      }
    }
    return out;
  }
  std::vector<std::string> DeletedIds() const {
    std::vector<std::string> ids;
    for (const Object& body : Bodies("delete")) {
      ids.push_back(body.getString("blob_id").value_or(""));
    }
    return ids;
  }
  uint64_t PutBytes() const {
    std::lock_guard lock(mutex_);
    return put_bytes_;
  }

private:
  Reply Answer(const ScriptedHttpServer::Request& request) {
    std::lock_guard lock(mutex_);
    if (request.method == "PUT") {
      put_bytes_ += request.body.size();
      return Reply{.status = put_status, .headers = {}, .body = ""};
    }
    const std::string prefix = "/api/relay/v1/files/";
    if (request.method != "POST" || request.target.rfind(prefix, 0) != 0) {
      return Reply{.status = 404, .headers = {}, .body = R"({"error":"Not found"})"};
    }
    const std::string op = request.target.substr(prefix.size());
    bodies_.emplace_back(op, TryParseObject(request.body).value_or(Object{}));
    const std::string blob_id = bodies_.back().second.getString("blob_id").value_or("");
    if (op == "start") {
      if (!start_replies.empty()) {
        Reply reply = start_replies.front();
        start_replies.erase(start_replies.begin());
        if (reply.status != 0) {
          return reply;
        }
      } else if (start_reply.status != 0) {
        return start_reply;
      }
      const std::string id = "aaaaaaaa-0000-4000-8000-" + std::string(11, '0') + std::to_string(++started_);
      return Reply{
          .status = 200,
          .headers = {},
          .body = R"({"blob_id":")" + id + R"(","mode":")" + start_mode +
                  R"(","part_bytes":16777216,"part_count":1,"upload_url":")" + server_.Origin() + "/s3/" + id +
                  R"(?X-Amz-Signature=abc","expires_in":900,"pending_expires_at":"2099-01-01T00:00:00.000Z"})"};
    }
    if (op == "complete") {
      if (complete_reply.status != 0) {
        return complete_reply;
      }
      return Reply{.status = 200,
                   .headers = {},
                   .body = R"({"blob_id":")" + blob_id +
                           R"(","token":"tok_0123456789abcdef","share_path":"/api/relay/v1/files/)" + blob_id +
                           R"(/download?t=tok_0123456789abcdef","expires_at":"2099-01-08T00:00:00.000Z"})"};
    }
    if (op == "delete") {
      return Reply{.status = 200, .headers = {}, .body = R"({"success":true})"};
    }
    if (op == "list") {
      return Reply{.status = 200, .headers = {}, .body = list_body};
    }
    return Reply{.status = 404, .headers = {}, .body = R"({"error":"Not found"})"};
  }

  ScriptedHttpServer& server_;
  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, Object>> bodies_;
  uint64_t put_bytes_ = 0;
  int started_ = 0;
};

/** The relay's `toISOString()` form of now minus `minutes`. */
std::string IsoMinutesAgo(const int minutes) {
  const time_t at = static_cast<time_t>(util::NowUnixMs() / 1000) - static_cast<time_t>(minutes) * 60;
  std::tm tm{};
  EXPECT_TRUE(civil_time::UtcTime(at, &tm));
  char text[32];
  std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return text;
}

Reply RelayError(const int status, const std::string& code, const std::string& text) {
  return Reply{.status = status, .headers = {}, .body = R"({"error":")" + text + R"(","code":")" + code + R"("})"};
}

class HttpBlobClientFilesTest : public ::testing::Test {
protected:
  HttpBlobClientFilesTest() : relay_(server_), client_(relay_.BaseUrl()) {
    client_.SetAuthSigner([this](const std::vector<uint8_t>& bytes) -> Roe<std::string> {
      signed_.push_back(BytesToHex(bytes));
      return std::string("sig");
    });
  }
  void SetUp() override {
    dir_ =
        std::filesystem::temp_directory_path() / ("pp_blob_files_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  std::string Staged(const size_t bytes) {
    const std::string path = (dir_ / "cipher.bin").string();
    std::ofstream(path, std::ios::binary) << std::string(bytes, 'c');
    return path;
  }
  Roe<RelayFileUpload> Upload(const size_t bytes) {
    return UploadRelayFile(client_, "relay:u1", Staged(bytes), bytes, {});
  }

  std::filesystem::path dir_;
  ScriptedHttpServer server_;
  FakeFilesRelay relay_;
  HttpBlobClient client_;
  std::vector<std::string> signed_;
};

TEST_F(HttpBlobClientFilesTest, StartPutCompleteGivesTheShareLinkOnTheRelayOrigin) {
  auto uploaded = Upload(5016);
  ASSERT_TRUE(uploaded) << uploaded.error().message;
  const auto starts = relay_.Bodies("start");
  ASSERT_EQ(starts.size(), 1u);
  EXPECT_EQ(starts[0].getNonNegInt("byte_length").value_or(0), 5016u) << "the ciphertext length is reserved";
  EXPECT_EQ(starts[0].getString("relay_user_id").value_or(""), "relay:u1");
  EXPECT_EQ(starts[0].getString("signature").value_or(""), "sig");
  EXPECT_EQ(relay_.PutBytes(), 5016u);
  ASSERT_EQ(relay_.Bodies("complete").size(), 1u);
  EXPECT_EQ(relay_.Bodies("complete")[0].getString("blob_id").value_or(""), uploaded->blob_id);
  EXPECT_EQ(uploaded->url,
            server_.Origin() + "/api/relay/v1/files/" + uploaded->blob_id + "/download?t=tok_0123456789abcdef");
  EXPECT_TRUE(relay_.DeletedIds().empty());
  // The signed bytes are the start domain's, for the timestamp the body carries.
  ASSERT_FALSE(signed_.empty());
  const auto ts = static_cast<int64_t>(starts[0].getNonNegInt("timestamp").value_or(0));
  EXPECT_EQ(signed_.front(), BytesToHex(BuildFileStartSignBytes("relay:u1", 5016, ts)));
}

TEST_F(HttpBlobClientFilesTest, BytesInMemoryGoTheSameWay) {
  auto uploaded = UploadRelayFileBytes(client_, "relay:u1", std::string(77, 'z'));
  ASSERT_TRUE(uploaded) << uploaded.error().message;
  EXPECT_EQ(relay_.Bodies("start")[0].getNonNegInt("byte_length").value_or(0), 77u);
  EXPECT_EQ(relay_.PutBytes(), 77u);
}

TEST_F(HttpBlobClientFilesTest, AMultipartAnswerIsAnErrorAndItsBlobIsDeleted) {
  relay_.start_mode = "multipart";
  auto uploaded = Upload(100);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(relay_.PutBytes(), 0u);
  EXPECT_EQ(relay_.DeletedIds().size(), 1u);
  EXPECT_TRUE(relay_.Bodies("complete").empty());
}

TEST_F(HttpBlobClientFilesTest, AFailedPutDeletesTheBlobAndReportsThePutError) {
  relay_.put_status = 403;
  auto uploaded = Upload(100);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(uploaded.error().code, static_cast<int32_t>(Err::Blob::UploadRejected));
  ASSERT_EQ(relay_.DeletedIds().size(), 1u);
  EXPECT_FALSE(relay_.DeletedIds()[0].empty());
  EXPECT_TRUE(relay_.Bodies("complete").empty());
}

TEST_F(HttpBlobClientFilesTest, AFailedCompleteDeletesTheBlobAndReportsTheCompleteError) {
  relay_.complete_reply = Reply{.status = 500, .headers = {}, .body = R"({"error":"Internal error"})"};
  auto uploaded = Upload(100);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(uploaded.error().code, static_cast<int32_t>(Err::Blob::Unavailable));
  ASSERT_EQ(relay_.DeletedIds().size(), 1u);
  EXPECT_EQ(relay_.DeletedIds()[0], relay_.Bodies("complete")[0].getString("blob_id").value_or("?"));
}

// complete answers once: BLOB_NOT_PENDING means an earlier complete went through but its answer (the token) was
// lost. The file can never be shared, so it goes.
TEST_F(HttpBlobClientFilesTest, BlobNotPendingOnCompleteDeletesTheBlob) {
  relay_.complete_reply = RelayError(400, "BLOB_NOT_PENDING", "Blob is not pending");
  auto uploaded = Upload(100);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(RelayFileErrorCode(uploaded.error()), "BLOB_NOT_PENDING");
  EXPECT_EQ(relay_.DeletedIds().size(), 1u);
}

TEST_F(HttpBlobClientFilesTest, TooManyPendingDeletesOnlyStalePendingAndStartsOnceMore) {
  relay_.start_replies = {RelayError(429, "QUOTA_PENDING_COUNT", "Too many pending uploads")};
  relay_.list_body = R"({"files":[
      {"blob_id":"old-1","status":"pending","byte_length":10,"created_at":"2020-01-01T00:00:00.000Z"},
      {"blob_id":"new-1","status":"pending","byte_length":10,"created_at":"2999-01-01T00:00:00.000Z"},
      {"blob_id":"half-hour","status":"pending","byte_length":10,"created_at":")" +
                     IsoMinutesAgo(30) + R"("},
      {"blob_id":"old-2","status":"pending","byte_length":10,"created_at":")" +
                     IsoMinutesAgo(70) + R"("}],
    "usage":{"bytes_used":30,"pending_count":20,"quota_bytes":1073741824,"max_pending":20}})";
  auto uploaded = Upload(100);
  ASSERT_TRUE(uploaded) << uploaded.error().message;
  ASSERT_EQ(relay_.Bodies("list").size(), 1u);
  EXPECT_EQ(relay_.Bodies("list")[0].getString("status").value_or(""), "pending");
  EXPECT_EQ(relay_.DeletedIds(), (std::vector<std::string>{"old-1", "old-2"}))
      << "an upload started in the last hour may be another device's, still going (a late PUT, a skewed clock)";
  EXPECT_EQ(relay_.Bodies("start").size(), 2u);
}

TEST_F(HttpBlobClientFilesTest, TooManyPendingWithNothingStaleIsReportedWithoutARetry) {
  relay_.start_reply = RelayError(429, "QUOTA_PENDING_COUNT", "Too many pending uploads");
  relay_.list_body = R"({"files":[{"blob_id":"new-1","status":"pending","created_at":"2999-01-01T00:00:00.000Z"}]})";
  auto uploaded = Upload(100);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(uploaded.error().code, static_cast<int32_t>(Err::Blob::RateLimited));
  EXPECT_TRUE(relay_.DeletedIds().empty());
  EXPECT_EQ(relay_.Bodies("start").size(), 1u);
}

TEST_F(HttpBlobClientFilesTest, AFullSpaceSaysSoAndNamesTheSizeItAskedFor) {
  relay_.start_reply = RelayError(429, "QUOTA_FILE_BYTES", "File storage quota exceeded");
  auto uploaded = Upload(4242);
  ASSERT_FALSE(uploaded);
  EXPECT_EQ(uploaded.error().code, static_cast<int32_t>(Err::Blob::QuotaExceeded));
  EXPECT_EQ(RelayFileErrorCode(uploaded.error()), "QUOTA_FILE_BYTES");
  EXPECT_NE(uploaded.error().message.find("byte_length=4242"), std::string::npos) << uploaded.error().message;
  EXPECT_TRUE(relay_.DeletedIds().empty()) << "no blob was started";
}

TEST_F(HttpBlobClientFilesTest, ListParsesFilesAndUsage) {
  relay_.list_body = R"({"files":[{"blob_id":"b1","status":"complete","mode":"single","byte_length":12,
      "download_count":1,"max_downloads":10,"created_at":"2026-11-01T00:00:00.000Z",
      "completed_at":"2026-11-01T00:00:01.000Z","expires_at":"2026-11-08T00:00:01.000Z"}],
    "usage":{"bytes_used":12,"pending_count":0,"quota_bytes":1073741824,"max_pending":20}})";
  auto listed = client_.FileList("relay:u1", "complete");
  ASSERT_TRUE(listed) << listed.error().message;
  ASSERT_EQ(listed->files.size(), 1u);
  EXPECT_EQ(listed->files[0].blob_id, "b1");
  EXPECT_EQ(listed->files[0].byte_length, 12u);
  EXPECT_EQ(listed->files[0].completed_at, "2026-11-01T00:00:01.000Z");
  EXPECT_EQ(listed->usage.bytes_used, 12u);
  EXPECT_EQ(listed->usage.quota_bytes, 1073741824u);
  EXPECT_EQ(listed->usage.max_pending, 20u);
  EXPECT_EQ(relay_.Bodies("list")[0].getString("status").value_or(""), "complete");
}

// The code table of the plan: the relay's `code` decides, the text is only log detail.
TEST_F(HttpBlobClientFilesTest, RelayCodesMapToTypedErrors) {
  const auto code_of = [](const int status, const std::string& code) {
    return ClassifyBlobFailure(status, "whatever", code).code;
  };
  EXPECT_EQ(code_of(400, "FILE_TOO_LARGE"), static_cast<int32_t>(Err::Blob::TooLarge));
  EXPECT_EQ(code_of(429, "QUOTA_FILE_BYTES"), static_cast<int32_t>(Err::Blob::QuotaExceeded));
  EXPECT_EQ(code_of(429, "QUOTA_PENDING_COUNT"), static_cast<int32_t>(Err::Blob::RateLimited));
  EXPECT_EQ(code_of(404, "BLOB_NOT_FOUND"), static_cast<int32_t>(Err::Blob::Expired));
  EXPECT_EQ(code_of(503, "UPLOAD_NOT_CONFIGURED"), static_cast<int32_t>(Err::Blob::UploadNotConfigured));
  EXPECT_EQ(code_of(403, "BLOB_NOT_OWNED"), static_cast<int32_t>(Err::Blob::UploadRejected));
  EXPECT_EQ(code_of(400, "BLOB_NOT_PENDING"), static_cast<int32_t>(Err::Blob::Generic));
  EXPECT_EQ(code_of(400, "UPLOAD_INCOMPLETE"), static_cast<int32_t>(Err::Blob::Generic));
  // No code (the /blobs calls): the status and text decide as before.
  EXPECT_EQ(code_of(429, ""), static_cast<int32_t>(Err::Blob::RateLimited));
  EXPECT_EQ(ClassifyBlobFailure(429, "QUOTA_FILE_BYTES", "QUOTA_FILE_BYTES").message,
            "HTTP 429 QUOTA_FILE_BYTES: QUOTA_FILE_BYTES");
  EXPECT_EQ(RelayFileErrorCode(ClassifyBlobFailure(400, "Blob is not pending", "BLOB_NOT_PENDING")),
            "BLOB_NOT_PENDING");
  EXPECT_EQ(RelayFileErrorCode(ClassifyBlobFailure(429, "Rate limit exceeded")), "");
  // FileStart notes the size it asked for after the code.
  EXPECT_EQ(RelayFileErrorCode(Error("HTTP 429 QUOTA_FILE_BYTES byte_length=123")), "QUOTA_FILE_BYTES");
  EXPECT_EQ(RelayFileErrorCode(Error("HTTP 429 QUOTA_FILE_BYTES")), "QUOTA_FILE_BYTES");
  EXPECT_EQ(RelayFileErrorCode(Error("HTTP 429 QUOTA_FILE_BYTESx")), "");
}

TEST(HttpBlobClientOriginTest, TheShareLinkOriginIsTheRelayBaseWithoutItsPath) {
  EXPECT_EQ(RelayOriginOf("https://www.brief.global/api/relay"), "https://www.brief.global");
  EXPECT_EQ(RelayOriginOf("https://www.brief.global/api/relay/"), "https://www.brief.global");
  EXPECT_EQ(RelayOriginOf("http://127.0.0.1:8080/api/relay"), "http://127.0.0.1:8080");
  EXPECT_EQ(RelayOriginOf("https://relay.example/other/base"), "https://relay.example");
}

} // namespace
} // namespace pbr
