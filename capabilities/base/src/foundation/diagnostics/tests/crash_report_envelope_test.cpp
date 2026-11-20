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

// release-compatibility "A build says what it is": the dump's commit and tag reach the envelope, outside the
// fingerprint (one crash in two builds of a version clusters together); a dump without them sends neither.
TEST(CrashReportEnvelopeTest, CarriesTheBuildsCommitAndTag) {
  const std::string head = "product=pp-browser\nversion=1.0.0\n";
  const std::string tail = "os=ios\nreason=signal 11\n0x7fff1234abcd\n--- breadcrumbs ---\n";
  const auto env = pbr::BuildCrashReportEnvelope(head + "commit=a1b2c3d4\ntag=v1.0.0-rc.3\n" + tail);
  EXPECT_EQ(env.version, "1.0.0");
  EXPECT_EQ(env.commit, "a1b2c3d4");
  EXPECT_EQ(env.tag, "v1.0.0-rc.3");
  EXPECT_EQ(env.os, "ios");
  const std::string json = pbr::CrashReportEnvelopeToJson(env);
  EXPECT_NE(json.find("\"commit\":\"a1b2c3d4\""), std::string::npos) << json;
  EXPECT_NE(json.find("\"tag\":\"v1.0.0-rc.3\""), std::string::npos) << json;

  const auto bare = pbr::BuildCrashReportEnvelope(head + tail);
  EXPECT_TRUE(bare.commit.empty());
  EXPECT_TRUE(bare.tag.empty());
  EXPECT_EQ(bare.fingerprint, env.fingerprint);
  const std::string bare_json = pbr::CrashReportEnvelopeToJson(bare);
  EXPECT_EQ(bare_json.find("\"commit\""), std::string::npos) << bare_json;
  EXPECT_EQ(bare_json.find("\"tag\""), std::string::npos) << bare_json;
}
