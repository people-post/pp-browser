#include "foundation/platform/CallKitDiagnostics.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace pbr {
namespace {

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

} // namespace

// The tone's lease release still closes its endpoint on the lane first; the diagnostic call comes after it.
TEST(CallAudioDiagnosticsTest, TheLeaseReleaseStillUnholdsAndClosesBeforeItLogs) {
  const std::string text = ReadFile("capabilities/media/src/lib/media/MediaDeviceArbiter.cpp");
  const size_t unhold = text.find("live->Unhold(id);");
  const size_t close = text.find("closing.reset();", unhold);
  const size_t note = text.find("AudioSession::NoteToneStreamClosed", unhold);
  ASSERT_NE(unhold, std::string::npos);
  ASSERT_NE(close, std::string::npos);
  ASSERT_NE(note, std::string::npos);
  EXPECT_LT(unhold, close);
  EXPECT_LT(close, note);
}

// The CallKit timing lines are written wherever a QA origin is compiled in (today every iOS build).
TEST(CallKitDiagnosticsGateTest, OffWithoutAQaAddressOnWithOne) {
  EXPECT_FALSE(CallKitDiagnosticsEnabled(""));
  EXPECT_FALSE(CallKitDiagnosticsEnabled(nullptr));
  EXPECT_TRUE(CallKitDiagnosticsEnabled("https://qa.example.test"));
}

} // namespace pbr
