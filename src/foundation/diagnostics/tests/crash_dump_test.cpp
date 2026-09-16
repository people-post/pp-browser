#include "foundation/diagnostics/CrashBreadcrumbs.h"
#include "foundation/diagnostics/CrashDump.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

std::string TempDir() {
  const auto base = std::filesystem::temp_directory_path() / "pp-browser-crash-dump-test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base.string();
}

} // namespace

TEST(CrashBreadcrumbs, SnapshotKeepsRecentLines) {
  for (int i = 0; i < 5; ++i) {
    pbr::CrashBreadcrumbs::Append("line-" + std::to_string(i));
  }
  const auto snap = pbr::CrashBreadcrumbs::Snapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_NE(snap.back().find("line-4"), std::string::npos);
}

TEST(CrashDump, WriteReadClearPending) {
  const std::string dir = TempDir();
  EXPECT_FALSE(pbr::CrashDump::HasPending(dir));

  pbr::CrashBreadcrumbs::Append("[E] CallStack: before-dump");
  pbr::CrashDump::WritePending(dir, "unit-test");
  ASSERT_TRUE(pbr::CrashDump::HasPending(dir));

  const std::string body = pbr::CrashDump::ReadPending(dir);
  EXPECT_NE(body.find("reason=unit-test"), std::string::npos);
  EXPECT_NE(body.find("before-dump"), std::string::npos);
  EXPECT_NE(body.find("version="), std::string::npos);
  EXPECT_NE(body.find("os="), std::string::npos);

  pbr::CrashDump::ClearPending(dir);
  EXPECT_FALSE(pbr::CrashDump::HasPending(dir));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(CrashReportEnvelope, BuildsFingerprintAndJson) {
  const std::string dump =
      "pp-browser crash dump\n"
      "product=pp-browser\n"
      "version=1.0.0-rc14\n"
      "os=linux\n"
      "reason=signal 11\n"
      "0x1000\n"
      "0x2000\n"
      "--- breadcrumbs ---\n"
      "[E] CallStack: hop failed\n"
      "[I] Application: tick\n"
      "[E] CallStack: again\n";

  const pbr::CrashReportEnvelope env = pbr::BuildCrashReportEnvelope(dump);
  EXPECT_EQ(env.version, "1.0.0-rc14");
  EXPECT_EQ(env.os, "linux");
  EXPECT_EQ(env.reason, "signal 11");
  ASSERT_EQ(env.frames.size(), 2u);
  EXPECT_EQ(env.frames[0], "0x1000");
  ASSERT_EQ(env.breadcrumb_tags.size(), 2u);
  EXPECT_EQ(env.breadcrumb_tags[0], "CallStack");
  EXPECT_EQ(env.breadcrumb_tags[1], "Application");
  EXPECT_FALSE(env.fingerprint.empty());

  const pbr::CrashReportEnvelope again = pbr::BuildCrashReportEnvelope(dump);
  EXPECT_EQ(env.fingerprint, again.fingerprint);

  const std::string json = pbr::CrashReportEnvelopeToJson(env);
  EXPECT_NE(json.find("\"fingerprint\""), std::string::npos);
  EXPECT_NE(json.find(env.fingerprint), std::string::npos);
}

TEST(CrashReportEnvelope, ResolveUrlPrefersOverride) {
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("https://www.brief.global/api/relay/",
                                             "https://override.example/crash"),
            "https://override.example/crash");
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("https://www.brief.global/api/relay/", ""),
            "https://www.brief.global/api/relay/v1/crash-reports");
  EXPECT_TRUE(pbr::ResolveCrashReportUploadUrl("", "").empty());
}
