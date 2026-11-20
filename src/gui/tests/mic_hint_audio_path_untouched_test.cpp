
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

// B233 step 1 says so when the microphone is not open and records what happened; it must not change how audio devices
// or the audio session are opened, closed or configured (the owner decided that repair waits for real device logs).
// These functions are pinned by a hash of their text (trailing blanks dropped, and the lines that only write a log —
// `SDL_Log(` / `EngineLog()` — left out, so a log line can still be added or moved to the file log). A change to any
// of them is a change to the audio path: do it on purpose (step 2) and update the hash in the same commit.

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

/** The function from `signature` to its matching closing brace (the whole file when `signature` is empty). */
std::string Slice(const std::string& text, const std::string& signature) {
  if (signature.empty()) {
    return text;
  }
  const size_t start = text.find(signature);
  if (start == std::string::npos) {
    return {};
  }
  size_t pos = text.find('{', start);
  int depth = 0;
  for (; pos < text.size(); ++pos) {
    if (text[pos] == '{') {
      ++depth;
    } else if (text[pos] == '}' && --depth == 0) {
      break;
    }
  }
  return text.substr(start, pos - start + 1);
}

uint64_t PinHash(const std::string& relative, const std::string& signature) {
  const std::string slice = Slice(ReadFile(relative), signature);
  uint64_t hash = kFnvOffset;
  std::istringstream lines(slice);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("SDL_Log(") != std::string::npos || line.find("EngineLog()") != std::string::npos) {
      continue;
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.pop_back();
    }
    line.push_back('\n');
    for (const unsigned char c : line) {
      hash = (hash ^ c) * kFnvPrime;
    }
  }
  return hash;
}

struct Pin {
  const char* file;
  const char* signature;
  uint64_t hash;
};

constexpr const char* kEngine = "capabilities/media/src/lib/media/MediaEngine.cpp";
constexpr const char* kIosSession = "capabilities/media/src/lib/media/AudioSession_Ios.mm";
constexpr const char* kDefaultSession = "capabilities/media/src/lib/media/AudioSession_Default.cpp";
constexpr const char* kAndroidSession = "capabilities/media/src/lib/media/AudioSession_Android.cpp";

const std::vector<Pin>& Pins() {
  static const std::vector<Pin> pins = {
      {kEngine, "void CloseAudioDevicesLocked() {", 0x204356a2a27978bfULL},
      {kEngine, "Roe<void> EnsureAudioLeases(bool reopen, bool retry_voice = false) {", 0x86a466f53458a528ULL},
      {kEngine, "void CloseMicBeforeRoute(AudioDeviceLease* mic) {", 0x66d030d8b484b3dbULL},
      {kEngine, "bool RunPendingRouteChange() {", 0x964d895fc0ebfa58ULL},
      {kEngine, "bool WaitedAudioGrantable() {", 0xaf6db6a8e57aa0d5ULL},
      {kEngine, "void TryAcquireVoiceDuplex(const SessionSpec& want_spec", 0xf13ace728e49ab9fULL},
      {kEngine, "void StartCaptureLoop() {", 0xf843200f3ce43f58ULL},
      {kIosSession, "void ConfigureVoipSession(AVAudioSession* session) {", 0x5ff40467380f469dULL},
      {kIosSession, "void ActivateForVoip() {", 0xe0d2082c13693967ULL},
      {kIosSession, "void Deactivate() {", 0x4f0fe447f7418a97ULL},
      {kIosSession, "void DeactivateAttempt(uint64_t gen, int attempts_left) {", 0x29c8282c6dabd728ULL},
      {kDefaultSession, "void ActivateForVoip() {", 0xc71dcde708149124ULL},
      {kDefaultSession, "void Deactivate() {", 0x4e73b55fcde1fdeaULL},
      {kAndroidSession, "void ActivateForVoip() {", 0x3e64d84ad2ca9c20ULL},
      {kAndroidSession, "void Deactivate() {", 0x048983ad01478ca2ULL},
      {"capabilities/media/src/lib/media/MediaDeviceArbiter.cpp", "void OpenOnDevice(IMediaDeviceBackend& backend) {",
       0xc17df3d8b827ce53ULL},
      {"capabilities/media/src/lib/media/VoiceProcessingIo_Apple.cpp", "", 0x82bf5b7ce2813f9cULL},
  };
  return pins;
}

} // namespace

TEST(MicHintAudioPathUntouchedTest, TheDeviceAndSessionFunctionsAreTheOnesBeforeTheHint) {
  for (const Pin& pin : Pins()) {
    ASSERT_FALSE(Slice(ReadFile(pin.file), pin.signature).empty()) << pin.file << " " << pin.signature;
    EXPECT_EQ(PinHash(pin.file, pin.signature), pin.hash) << pin.file << " " << pin.signature;
  }
}

// What the engine does to answer "is a microphone wanted but not open" is look: HealthSnapshot calls none of the
// functions that open, close, reopen or configure anything.
TEST(MicHintAudioPathUntouchedTest, TheHealthSnapshotOnlyLooks) {
  const std::string body = Slice(ReadFile(kEngine), "MediaEngineHealth MediaEngine::HealthSnapshot() const {");
  ASSERT_FALSE(body.empty());
  for (const char* forbidden :
       {"EnsureAudioLeases", "CloseAudioDevicesLocked", "CloseMicBeforeRoute", "RunPendingRouteChange",
        "TryAcquireVoiceDuplex", "AcquireAudio", "Reopen", "reopen", "ActivateForVoip", "Deactivate",
        "RequestAudioRouteChange", "audio_reopen_requested", "audio_waiting", "capture_running", "std::thread",
        ".reset()", "SetMuted", "NoteRouteChanged", "RequestMicPermission"}) {
    EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
  }
  // The one OS question it asks: the microphone permission, read without opening anything.
  EXPECT_NE(body.find("MicPermissionState()"), std::string::npos);
}

// The call page answers a missing microphone only by words: CallController's new notice case takes a pill down by its
// text and does nothing to audio.
TEST(MicHintAudioPathUntouchedTest, TheWithdrawNoticeOnlyTakesDownItsOwnPill) {
  const std::string text = ReadFile("src/gui/CallController.cpp");
  const size_t at = text.find("case CallNotice::Kind::HintWithdrawn:");
  ASSERT_NE(at, std::string::npos);
  const std::string tail = text.substr(at, 220);
  EXPECT_NE(tail.find("UserFeedback::DismissToastWithMessage(Tr(notice.key))"), std::string::npos) << tail;
  EXPECT_EQ(tail.find("DismissToast()"), std::string::npos) << "a blind dismiss would take down another pill";
}

} // namespace pbr
