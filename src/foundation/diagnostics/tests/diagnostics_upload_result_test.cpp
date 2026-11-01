#include "foundation/diagnostics/DiagnosticsUploadResult.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <sys/stat.h>

namespace pbr {
namespace {

using Kind = DiagnosticsUploadKind;

TEST(DiagnosticsUploadResultTest, SuccessCarriesTheReferenceAndLineCount) {
  const auto r = MapDiagnosticsResponse(200, R"({"upload_id":"ABCDEFGH12","lines":42,"deduplicated":false})");
  EXPECT_EQ(r.kind, Kind::Sent);
  EXPECT_EQ(r.upload_id, "ABCDEFGH12");
  EXPECT_EQ(r.lines, 42);
}

TEST(DiagnosticsUploadResultTest, SuccessWithoutAReferenceIsAFailure) {
  EXPECT_EQ(MapDiagnosticsResponse(200, "{}").kind, Kind::Failed);
  EXPECT_EQ(MapDiagnosticsResponse(200, "not json").kind, Kind::Failed);
}

TEST(DiagnosticsUploadResultTest, RateLimitReasonsMapToTheirKind) {
  EXPECT_EQ(MapDiagnosticsResponse(429, R"({"error":"rate_limited","reason":"quota_key"})").kind, Kind::QuotaKey);
  EXPECT_EQ(MapDiagnosticsResponse(429, R"({"error":"rate_limited","reason":"quota_ip"})").kind, Kind::QuotaIp);
  EXPECT_EQ(MapDiagnosticsResponse(429, R"({"error":"rate_limited","reason":"quota_global"})").kind,
            Kind::QuotaGlobal);
  const auto unknown = MapDiagnosticsResponse(429, R"({"error":"rate_limited","reason":"new_reason"})");
  EXPECT_EQ(unknown.kind, Kind::Failed);
  EXPECT_EQ(MapDiagnosticsResponse(429, "").kind, Kind::Failed);
}

TEST(DiagnosticsUploadResultTest, OtherStatusesMap) {
  EXPECT_EQ(MapDiagnosticsResponse(409, "{}").kind, Kind::InProgress);
  EXPECT_EQ(MapDiagnosticsResponse(401, "{}").kind, Kind::Unavailable);
  EXPECT_EQ(MapDiagnosticsResponse(404, "").kind, Kind::Unavailable);
  EXPECT_EQ(MapDiagnosticsResponse(503, "").kind, Kind::Unavailable);
  EXPECT_EQ(MapDiagnosticsResponse(413, "").kind, Kind::TooLarge);
  const auto other = MapDiagnosticsResponse(500, "oops");
  EXPECT_EQ(other.kind, Kind::Failed);
  EXPECT_EQ(other.detail, "HTTP 500");
}

TEST(DiagnosticsUploadResultTest, TokenIsReusedOnlyAfterAnAttemptWithoutAResult) {
  for (const Kind keeps : {Kind::NoNetwork, Kind::Failed, Kind::InProgress}) {
    DiagnosticsClientToken token;
    const std::string first = token.Acquire();
    EXPECT_FALSE(first.empty());
    token.Finish(keeps);
    EXPECT_EQ(token.Acquire(), first);
  }
  for (const Kind drops : {Kind::Sent, Kind::SelfCheckRefused, Kind::QuotaKey, Kind::QuotaIp, Kind::QuotaGlobal,
                           Kind::TooLarge, Kind::Unavailable}) {
    DiagnosticsClientToken token;
    const std::string first = token.Acquire();
    token.Finish(drops);
    EXPECT_NE(token.Acquire(), first);
  }
}

TEST(DiagnosticsUploadResultTest, TokenIsStableWithinOneAttempt) {
  DiagnosticsClientToken token;
  EXPECT_EQ(token.Acquire(), token.Acquire());
}

TEST(DiagnosticsUploadResultTest, LastUploadRoundTripsOwnerOnly) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_diag_last_upload_test";
  std::filesystem::remove_all(dir);
  EXPECT_FALSE(ReadLastUpload(dir.string()).has_value());

  ASSERT_TRUE(WriteLastUpload(dir.string(), {"ABCDEFGH12", 1761600000000}));
  const auto read = ReadLastUpload(dir.string());
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->upload_id, "ABCDEFGH12");
  EXPECT_EQ(read->at_ms, 1761600000000);

  ASSERT_TRUE(WriteLastUpload(dir.string(), {"ZZZZZZZZ99", 5}));  // newer replaces older
  EXPECT_EQ(ReadLastUpload(dir.string())->upload_id, "ZZZZZZZZ99");

#ifndef _WIN32
  struct stat st {};
  ASSERT_EQ(::stat(LastUploadPath(dir.string()).c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
#endif
  std::filesystem::remove_all(dir);
}

TEST(DiagnosticsUploadResultTest, CorruptLastUploadReadsAsNone) {
  const auto dir = std::filesystem::temp_directory_path() / "pp_diag_last_upload_bad";
  std::filesystem::remove_all(dir);
  ASSERT_TRUE(WriteLastUpload(dir.string(), {"X", 1}));
  std::filesystem::resize_file(LastUploadPath(dir.string()), 3);
  EXPECT_FALSE(ReadLastUpload(dir.string()).has_value());
  std::filesystem::remove_all(dir);
}

} // namespace
} // namespace pbr
