#include "foundation/diagnostics/DiagnosticsBundle.h"

#include "common/ValueJson.h"
#include "foundation/diagnostics/DiagnosticsScrub.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

namespace fs = std::filesystem;

fs::path Fixture(const char* name) { return fs::path(PP_BROWSER_DIAG_FIXTURES_DIR) / name; }

std::string ReadAll(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

pbr::DiagnosticsBundleInput FixtureInput() {
  pbr::DiagnosticsBundleInput in;
  in.log_files = {Fixture("diag_sample.log")};
  in.metrics_file = Fixture("diag_metrics.log");
  pbr::CrashReportEnvelope env;
  env.reason = "SIGSEGV";
  env.frames = {"0x00007ff812345678", "pp::Foo::bar+0x10"};
  env.breadcrumb_tags = {"MeshLink"};
  env.sample = "reason=SIGSEGV\nhost 203.0.113.9:4001 account:AbCdEfGhIjKlMnOpQrStUvWxYz0123456789-_AbCdE\n";
  in.crash_lines = pbr::CrashEnvelopeLines(env, "2026-10-28 09:00:00.000");
  in.client_token = "tok-1";
  in.product = "pp-browser";
  in.version = "1.0.0";
  in.os = "macos";
  in.captured_at_ms = 1761642000000;
  return in;
}

/** "## name" then the lines of each file, for comparing against the expected fixture. */
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

fs::path TempFile(const char* name, const std::string& content) {
  const fs::path p = fs::temp_directory_path() / (std::string("pp_diag_test_") + name);
  std::ofstream(p, std::ios::binary) << content;
  return p;
}

}  // namespace

TEST(DiagnosticsBundleTest, OutputMatchesExpectedFixture) {
  auto bundle = pbr::BuildDiagnosticsBundle(FixtureInput());
  ASSERT_TRUE(static_cast<bool>(bundle)) << bundle.error().message;
  EXPECT_EQ(Render(bundle->json), ReadAll(Fixture("diag_sample.expected.log")));
}

TEST(DiagnosticsBundleTest, FilteringAndScrubbingProperties) {
  auto bundle = pbr::BuildDiagnosticsBundle(FixtureInput());
  ASSERT_TRUE(static_cast<bool>(bundle));
  const std::string text = Render(bundle->json);
  EXPECT_EQ(text.find("[DEBUG]"), std::string::npos);
  EXPECT_EQ(text.find("user typed hello"), std::string::npos);
  EXPECT_EQ(text.find("NotARealChannel"), std::string::npos);
  EXPECT_EQ(text.find("continuation"), std::string::npos);
  EXPECT_EQ(text.find("jane"), std::string::npos);
  EXPECT_EQ(text.find("203.0.113.9"), std::string::npos);
  EXPECT_NE(text.find("[Metrics] call.setup ms=123"), std::string::npos);
  EXPECT_NE(text.find("[Crash] reason=SIGSEGV"), std::string::npos);
  EXPECT_NE(text.find("08:57:32.329"), std::string::npos);
  EXPECT_NE(text.find("elapsed 01:23"), std::string::npos);
  // The forged prefix is inside one kept line, never a line of its own.
  EXPECT_EQ(text.find("\n[2026-10-28 08:57:59.000]"), std::string::npos);
  EXPECT_EQ(bundle->bytes, bundle->json.size());
  EXPECT_GT(bundle->lines, 5);
}

TEST(DiagnosticsBundleTest, CrashEnvelopeLinesAreOnePhysicalLineEach) {
  pbr::CrashReportEnvelope env;
  env.reason = "r";
  env.frames = {"f1"};
  env.breadcrumb_tags = {"T"};
  env.sample = "a\nb";
  const auto lines = pbr::CrashEnvelopeLines(env, "2026-10-28 09:00:00.000");
  ASSERT_EQ(lines.size(), 4u);
  EXPECT_EQ(lines[0], "[2026-10-28 09:00:00.000] [INFO] [Crash] reason=r");
  EXPECT_EQ(lines[1], "[2026-10-28 09:00:00.000] [INFO] [Crash] frame=f1");
  EXPECT_EQ(lines[2], "[2026-10-28 09:00:00.000] [INFO] [Crash] tag=T");
  EXPECT_EQ(lines[3], "[2026-10-28 09:00:00.000] [INFO] [Crash] sample=a\\nb");
}

TEST(DiagnosticsBundleTest, ByteCapKeepsNewestLines) {
  std::string big;
  // Quote/backslash-dense lines roughly double when escaped; ~6 MiB raw.
  const std::string filler(900, '\\');
  for (int i = 0; i < 6000; ++i) {
    char ts[64];
    std::snprintf(ts, sizeof(ts), "[2026-10-28 08:%02d:%02d.000] [INFO] [Application] ", (i / 60) % 60, i % 60);
    big += ts + std::string("n=") + std::to_string(i) + " " + filler + "\n";
  }
  const fs::path p = TempFile("big.log", big);
  pbr::DiagnosticsBundleInput in;
  in.log_files = {p};
  in.client_token = "t";
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  fs::remove(p);
  ASSERT_TRUE(static_cast<bool>(bundle)) << bundle.error().message;
  EXPECT_LE(bundle->json.size(), 2u * 1024u * 1024u);
  EXPECT_GT(bundle->json.size(), 1u * 1024u * 1024u);
  EXPECT_NE(bundle->json.find("n=5999 "), std::string::npos);
  EXPECT_EQ(bundle->json.find("n=0 "), std::string::npos);
  EXPECT_LT(bundle->lines, 6000);
}

TEST(DiagnosticsBundleTest, LineCountCap) {
  std::string big;
  for (int i = 0; i < 31000; ++i) {
    big += "[2026-10-28 08:00:00.000] [INFO] [Application] n=" + std::to_string(i) + "\n";
  }
  const fs::path p = TempFile("many.log", big);
  pbr::DiagnosticsBundleInput in;
  in.log_files = {p};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  fs::remove(p);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle->lines, 30000);
  EXPECT_NE(bundle->json.find("n=30999\""), std::string::npos);
  EXPECT_EQ(bundle->json.find("n=999\""), std::string::npos);
}

TEST(DiagnosticsBundleTest, OldestFileIsDroppedFirst) {
  const std::string filler(900, '\\');
  auto make = [&](const char* tag, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) {
      s += "[2026-10-28 08:00:00.000] [INFO] [Application] " + std::string(tag) + std::to_string(i) + " " + filler + "\n";
    }
    return s;
  };
  const fs::path newest = TempFile("newest.log", make("NEW", 800));
  const fs::path oldest = TempFile("oldest.log", make("OLD", 800));
  pbr::DiagnosticsBundleInput in;
  in.log_files = {newest, oldest};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  fs::remove(newest);
  fs::remove(oldest);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_LE(bundle->json.size(), 2u * 1024u * 1024u);
  EXPECT_NE(bundle->json.find("NEW799 "), std::string::npos);
  EXPECT_NE(bundle->json.find("NEW0 "), std::string::npos);
  EXPECT_EQ(bundle->json.find("OLD0 "), std::string::npos);
}

TEST(DiagnosticsBundleTest, LongLineIsCutAt2048) {
  const fs::path p = TempFile("long.log", "[2026-10-28 08:00:00.000] [INFO] [Application] " + std::string(5000, 'x') + "\n");
  pbr::DiagnosticsBundleInput in;
  in.log_files = {p};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  fs::remove(p);
  ASSERT_TRUE(static_cast<bool>(bundle));
  const std::string text = Render(bundle->json);
  const std::string line = text.substr(text.find('\n') + 1);
  EXPECT_NE(line.find("[cut]"), std::string::npos);
  EXPECT_LE(line.size(), pbr::kDiagnosticsMaxLineBytes + 1u);  // + the newline Render adds
}

TEST(DiagnosticsBundleTest, ScrubbedFixturePassesSelfCheck) {
  auto bundle = pbr::BuildDiagnosticsBundle(FixtureInput());
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_EQ(pbr::SelfCheck(Render(bundle->json), {}), std::nullopt);
}

TEST(DiagnosticsBundleTest, UnexpectedIdentifierFormatBlocksTheBundle) {
  pbr::DiagnosticsBundleInput in;
  in.log_files = {Fixture("diag_unexpected.log")};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_FALSE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle.error().message, "self-check: hex_run");
}

TEST(DiagnosticsBundleTest, KnownIdentifierPrefixBlocksTheBundle) {
  auto in = FixtureInput();
  in.known_identifiers = {"Alice-secret-device"};  // "Alice-se" occurs in no line
  EXPECT_TRUE(static_cast<bool>(pbr::BuildDiagnosticsBundle(in)));
  in.known_identifiers = {"Started-and-more"};  // "Started-" is not present either
  EXPECT_TRUE(static_cast<bool>(pbr::BuildDiagnosticsBundle(in)));
  in.known_identifiers = {"elapsed 01:23 unique"};  // first 8 chars "elapsed " occur in a kept line
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_FALSE(static_cast<bool>(bundle));
  EXPECT_EQ(bundle.error().message, "self-check: known_identifier");
}

TEST(DiagnosticsBundleTest, UnscrubbedFixtureFiresSelfCheck) {
  EXPECT_TRUE(pbr::SelfCheck(ReadAll(Fixture("diag_sample.log")), {}).has_value());
}

TEST(DiagnosticsBundleTest, MissingFilesStillGiveOneFileEntry) {
  pbr::DiagnosticsBundleInput in;
  in.log_files = {Fixture("does_not_exist.log")};
  auto bundle = pbr::BuildDiagnosticsBundle(in);
  ASSERT_TRUE(static_cast<bool>(bundle));
  EXPECT_NE(bundle->json.find("\"files\""), std::string::npos);
  EXPECT_NE(bundle->json.find("does_not_exist.log"), std::string::npos);
  EXPECT_EQ(bundle->lines, 0);
}
