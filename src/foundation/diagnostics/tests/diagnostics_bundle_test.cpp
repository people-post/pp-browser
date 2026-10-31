#include "foundation/diagnostics/DiagnosticsBundle.h"

#include "common/ValueJson.h"
#include "foundation/diagnostics/DiagnosticsSelfCheck.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;

/** One directory per test, removed afterwards. */
class DiagnosticsBundleTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           (std::string("pp_diag_bundle_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_, ec);
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(dir_, ec);
  }
  fs::path Write(const char* name, const std::string& content) {
    const fs::path p = dir_ / name;
    std::ofstream(p, std::ios::binary) << content;
    return p;
  }
  fs::path dir_;
};

/** "## name" then the lines of each file. */
std::string Render(const std::string& json) {
  auto parsed = pp::common::ParseObject(json);
  EXPECT_TRUE(static_cast<bool>(parsed));
  std::string out;
  if (!parsed) return out;
  const auto* files = parsed->getArray("files");
  if (files == nullptr) return out;
  for (const auto& f : files->elements) {
    const pp::common::Object* file = pp::common::asObject(f);
    out += "## " + file->getString("name").value_or("") + "\n";
    const auto* lines = file->getArray("lines");
    for (const auto& l : lines->elements) out += pp::common::asString(l).value_or("") + "\n";
  }
  return out;
}

std::string Line(const char* level, const char* channel, const std::string& body) {
  return std::string("[2026-10-28 08:57:32.329] [") + level + "] [" + channel + "] " + body + "\n";
}

}  // namespace

// Only the shareable channels leave the device: a developer line that found its way into a channel file
// (a handler mix-up, a forged prefix) is dropped, never rewritten and sent.
TEST_F(DiagnosticsBundleTest, OnlyShareableChannelLinesAreSent) {
  const fs::path diag =
      Write("diag.log", Line("INFO", "Diag", "event=relay.http_failed action=send status=503") +
                            Line("WARNING", "Diag", "event=unlock.failed error=7.1") +
                            Line("DEBUG", "Diag", "event=too.chatty") +
                            Line("INFO", "ConversationsHub", "renamed contact to Jane Doe") +
                            "a continuation line without a prefix\n" +
                            Line("INFO", "Diag", "event=x note=[2026-10-28 08:57:59.000] [INFO] [Diag] forged"));
  const fs::path metrics = Write("metrics.log", Line("INFO", "Metrics", "event=call.setup ms=123"));
  pbr::DiagnosticsBundleInput in;
  in.files = {diag, metrics};
  pbr::CrashReportEnvelope env;
  env.reason = "SIGSEGV";
  env.frames = {"pp::Foo::bar+0x10"};
  env.sample = "raw dump text with a name: Jane";
  in.crash_lines = pbr::CrashEnvelopeLines(env, "2026-10-28 09:00:00.000");

  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle)) << bundle.error().message;
  const std::string text = Render(bundle->json);
  EXPECT_NE(text.find("[Diag] event=relay.http_failed action=send status=503"), std::string::npos) << text;
  EXPECT_NE(text.find("[Diag] event=unlock.failed error=7.1"), std::string::npos);
  EXPECT_NE(text.find("[Metrics] event=call.setup ms=123"), std::string::npos);
  EXPECT_NE(text.find("[Crash] reason=SIGSEGV"), std::string::npos);
  EXPECT_EQ(text.find("too.chatty"), std::string::npos) << "below INFO";
  EXPECT_EQ(text.find("Jane"), std::string::npos) << "another channel's line, or the crash sample";
  EXPECT_EQ(text.find("continuation"), std::string::npos);
  EXPECT_EQ(text.find("\n[2026-10-28 08:57:59.000]"), std::string::npos) << "a forged prefix is never a line";
  EXPECT_EQ(bundle->lines, 6);  // three Diag lines, one Metrics line, the crash reason and frame
  EXPECT_EQ(bundle->bytes, bundle->json.size());
}

TEST(DiagnosticsBundleCrashTest, CrashEnvelopeLinesAreOnePhysicalLineEachWithoutTheSample) {
  pbr::CrashReportEnvelope env;
  env.reason = "r\nr";
  env.frames = {"f1"};
  env.breadcrumb_tags = {"T"};
  env.sample = "a\nb";
  const auto lines = pbr::CrashEnvelopeLines(env, "2026-10-28 09:00:00.000");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "[2026-10-28 09:00:00.000] [INFO] [Crash] reason=r\\nr");
  EXPECT_EQ(lines[1], "[2026-10-28 09:00:00.000] [INFO] [Crash] frame=f1");
  EXPECT_EQ(lines[2], "[2026-10-28 09:00:00.000] [INFO] [Crash] tag=T");
}

TEST_F(DiagnosticsBundleTest, ByteCapKeepsNewestLines) {
  std::string big;
  const std::string filler(900, 'z');
  for (int i = 0; i < 6000; ++i) {
    char ts[64];
    std::snprintf(ts, sizeof(ts), "[2026-10-28 08:%02d:%02d.000] [INFO] [Diag] ", (i / 60) % 60, i % 60);
    big += ts + std::string("event=e n=") + std::to_string(i) + " pad=" + filler + "\n";
  }
  pbr::DiagnosticsBundleInput in;
  in.files = {Write("diag.log", big)};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle)) << bundle.error().message;
  EXPECT_LE(bundle->json.size(), pbr::kDiagnosticsMaxJsonBytes);
  EXPECT_GT(bundle->json.size(), pbr::kDiagnosticsMaxJsonBytes / 2);
  EXPECT_NE(bundle->json.find("n=5999 "), std::string::npos);
  EXPECT_EQ(bundle->json.find("n=0 "), std::string::npos);
  EXPECT_LT(bundle->lines, 6000);
}

TEST_F(DiagnosticsBundleTest, LineCountCap) {
  std::string big;
  for (int i = 0; i < 31000; ++i) {
    big += "[2026-10-28 08:00:00.000] [INFO] [Diag] event=e n=" + std::to_string(i) + "\n";
  }
  pbr::DiagnosticsBundleInput in;
  in.files = {Write("diag.log", big)};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle->lines, pbr::kDiagnosticsMaxLines);
  EXPECT_NE(bundle->json.find("n=30999\""), std::string::npos);
  EXPECT_EQ(bundle->json.find("n=999\""), std::string::npos);
}

TEST_F(DiagnosticsBundleTest, TheLeastWantedFileIsDroppedFirst) {
  const std::string filler(900, 'z');
  auto make = [&](const char* tag, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) {
      s += "[2026-10-28 08:00:00.000] [INFO] [Diag] event=" + std::string(tag) + std::to_string(i) + " pad=" +
           filler + "\n";
    }
    return s;
  };
  pbr::DiagnosticsBundleInput in;
  in.files = {Write("diag.log", make("NEW", 1200)), Write("diag.1.log", make("OLD", 1200))};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_LE(bundle->json.size(), pbr::kDiagnosticsMaxJsonBytes);
  EXPECT_NE(bundle->json.find("NEW1199 "), std::string::npos);
  EXPECT_NE(bundle->json.find("NEW0 "), std::string::npos);
  EXPECT_EQ(bundle->json.find("OLD0 "), std::string::npos);
}

TEST_F(DiagnosticsBundleTest, LongLineIsCut) {
  pbr::DiagnosticsBundleInput in;
  in.files = {Write("diag.log", "[2026-10-28 08:00:00.000] [INFO] [Diag] event=e pad=" + std::string(5000, 'z') + "\n")};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle));
  const std::string text = Render(bundle->json);
  const std::string line = text.substr(text.find('\n') + 1);
  EXPECT_NE(line.find("[cut]"), std::string::npos);
  EXPECT_LE(line.size(), pbr::kDiagnosticsMaxLineBytes + 1u);  // + the newline Render adds
}

// The last gate: a line that got past the shareable-line rule stops the whole send.
TEST_F(DiagnosticsBundleTest, ASelfCheckHitSendsNothing) {
  pbr::DiagnosticsBundleInput in;
  in.files = {Write("diag.log", Line("INFO", "Diag", "event=x legacy=9f86d081884c7d659a2feaa0c55ad015a3bf4f1b"))};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_FALSE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle.error().message, "self-check: hex_run");

  in.files = {Write("diag.log", Line("INFO", "Diag", "event=x relay=relay-eu-1"))};
  in.known_identifiers = {"relay:relay-eu-1.example.net"};
  bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_FALSE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle.error().message, "self-check: known_identifier");
}

TEST_F(DiagnosticsBundleTest, MissingFilesStillGiveOneFileEntry) {
  pbr::DiagnosticsBundleInput in;
  in.files = {dir_ / "diag.log"};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_NE(bundle->json.find("\"files\""), std::string::npos);
  EXPECT_NE(bundle->json.find("diag.log"), std::string::npos);
  EXPECT_EQ(bundle->lines, 0);
}
