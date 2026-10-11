#include "foundation/diagnostics/CrashReportEnvelope.h"

#include <gtest/gtest.h>

#include <string>

// PB-038: the upload URL may come from a cached server document; only an https URL is taken from it,
// anything else falls back to the relay base.
TEST(CrashReportEnvelopeTest, UploadUrlOverrideMustBeHttps) {
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("https://relay.example/", "https://crash.example/v1/reports"),
            "https://crash.example/v1/reports");
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("https://relay.example/", "http://crash.example/v1/reports"),
            "https://relay.example/v1/crash-reports");
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("https://relay.example/", "ftp://crash.example/x"),
            "https://relay.example/v1/crash-reports");
  EXPECT_EQ(pbr::ResolveCrashReportUploadUrl("", "http://crash.example/v1/reports"), "");
}

// PB-038: the sample sent with a report holds the crash metadata and frames only. Breadcrumbs are
// recent log lines and may carry ids and addresses; only their logger tags leave the device.
TEST(CrashReportEnvelopeTest, SampleStopsBeforeBreadcrumbs) {
  const std::string dump =
      "product=pp-browser\nversion=1.0\nos=macos\nreason=SIGSEGV\n"
      "0x7fff1234abcd\n"
      "--- breadcrumbs ---\n"
      "[E] MeshLink: peer=QmSecretPeerId addr=/ip4/203.0.113.9\n"
      "[W] ChatController: thread=contact:QmOther\n";
  const auto env = pbr::BuildCrashReportEnvelope(dump);
  EXPECT_EQ(env.reason, "SIGSEGV");
  ASSERT_EQ(env.frames.size(), 1u);
  EXPECT_EQ(env.sample.find("QmSecretPeerId"), std::string::npos) << env.sample;
  EXPECT_EQ(env.sample.find("203.0.113.9"), std::string::npos) << env.sample;
  EXPECT_NE(env.sample.find("reason=SIGSEGV"), std::string::npos) << env.sample;
  EXPECT_NE(env.sample.find("0x7fff1234abcd"), std::string::npos) << env.sample;
  ASSERT_EQ(env.breadcrumb_tags.size(), 2u);
  EXPECT_EQ(env.breadcrumb_tags[0], "MeshLink");
}
