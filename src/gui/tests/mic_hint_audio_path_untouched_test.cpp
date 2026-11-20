
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace pbr {
namespace {

// TODO(B233): remove this file once step 1 is merged and step 2 (automatic repair, which changes these functions on
// purpose) starts.
//
// B233 step 1 says so when the microphone is not open and records what happened; it must not change how audio devices
// or the audio session are opened, closed or configured (the owner decided that repair waits for real device logs).
// These functions are pinned by a hash of their code: comments and all whitespace are ignored (so formatting cannot
// trip it) and so is every log statement (`SDL_Log(...)` / `EngineLog()...;`, however many lines), so a log line can
// still be added, moved or sent to the file log. If you changed one of them on purpose, update the expected value in
// the table below, in the same commit.

constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

std::string ReadFile(const std::string& relative) {
  std::ifstream in(std::filesystem::path(PP_SOURCE_DIR) / relative, std::ios::binary);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

/** Index just past the string or character literal that starts at `i` (text[i] is its opening quote). */
size_t SkipLiteral(const std::string& text, size_t i) {
  const char quote = text[i++];
  while (i < text.size() && text[i] != quote) {
    i += text[i] == '\\' ? 2 : 1;
  }
  return i + 1;
}

/** A quote after a letter or digit is a digit separator (1'000), not a character literal. */
bool StartsLiteral(const std::string& text, size_t i) {
  if (text[i] == '"') {
    return true;
  }
  return text[i] == '\'' && (i == 0 || !std::isalnum(static_cast<unsigned char>(text[i - 1])));
}

std::string StripComments(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    if (StartsLiteral(text, i)) {
      const size_t end = std::min(SkipLiteral(text, i), text.size());
      out.append(text, i, end - i);
      i = end;
    } else if (text.compare(i, 2, "//") == 0) {
      while (i < text.size() && text[i] != '\n') {
        ++i;
      }
    } else if (text.compare(i, 2, "/*") == 0) {
      const size_t end = text.find("*/", i + 2);
      i = end == std::string::npos ? text.size() : end + 2;
      out.push_back(' ');
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

/** Index just past the `;` that ends the statement starting at `i` (a `;` inside a literal does not end it). */
size_t EndOfStatement(const std::string& text, size_t i) {
  while (i < text.size() && text[i] != ';') {
    i = StartsLiteral(text, i) ? SkipLiteral(text, i) : i + 1;
  }
  return std::min(i + 1, text.size());
}

std::string StripLogStatements(const std::string& text) {
  std::string out;
  for (size_t i = 0; i < text.size();) {
    if (text.compare(i, 8, "SDL_Log(") == 0 || text.compare(i, 12, "EngineLog().") == 0) {
      i = EndOfStatement(text, i);
    } else if (StartsLiteral(text, i)) {
      const size_t end = std::min(SkipLiteral(text, i), text.size());
      out.append(text, i, end - i);
      i = end;
    } else {
      out.push_back(text[i++]);
    }
  }
  return out;
}

/** The function from `signature` to its matching closing brace (the whole text when `signature` is empty). */
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
  while (pos < text.size()) {
    if (StartsLiteral(text, pos)) {
      pos = SkipLiteral(text, pos);
      continue;
    }
    if (text[pos] == '{') {
      ++depth;
    } else if (text[pos] == '}' && --depth == 0) {
      break;
    }
    ++pos;
  }
  return text.substr(start, std::min(pos, text.size() - 1) - start + 1);
}

uint64_t PinHash(const std::string& relative, const std::string& signature) {
  const std::string slice = Slice(StripLogStatements(StripComments(ReadFile(relative))), signature);
  uint64_t hash = kFnvOffset;
  for (const unsigned char c : slice) {
    if (!std::isspace(c)) {
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
      {kEngine, "void CloseAudioDevicesLocked() {", 0x14e6d0e00487cab3ULL},
      {kEngine, "Roe<void> EnsureAudioLeases(bool reopen, bool retry_voice = false) {", 0x7519d048169f5322ULL},
      {kEngine, "void CloseMicBeforeRoute(AudioDeviceLease* mic) {", 0x9f6ceddd69ca1845ULL},
      {kEngine, "bool RunPendingRouteChange() {", 0x2b80b97a92904f54ULL},
      {kEngine, "bool WaitedAudioGrantable() {", 0xf6ade8b9aa534811ULL},
      {kEngine, "void TryAcquireVoiceDuplex(const SessionSpec& want_spec", 0xc806268a86bb0efbULL},
      {kEngine, "void StartCaptureLoop() {", 0x1279ef63e56708a8ULL},
      {kIosSession, "void ConfigureVoipSession(AVAudioSession* session) {", 0x62ea8bfcaff0241eULL},
      {kIosSession, "void ActivateForVoip() {", 0xe3e90388299ad245ULL},
      {kIosSession, "void Deactivate() {", 0xd0d1bfba5214f980ULL},
      {kIosSession, "void DeactivateAttempt(uint64_t gen, int attempts_left) {", 0xe9b2d00dcb12ab7aULL},
      {kDefaultSession, "void ActivateForVoip() {", 0x864369fd8532b878ULL},
      {kDefaultSession, "void Deactivate() {", 0x280deb40a75327b2ULL},
      {kAndroidSession, "void ActivateForVoip() {", 0xba1666222b4c825eULL},
      {kAndroidSession, "void Deactivate() {", 0x35f129af3a12b17eULL},
      {"capabilities/media/src/lib/media/MediaDeviceArbiter.cpp", "void OpenOnDevice(IMediaDeviceBackend& backend) {",
       0x6e26531375e5411eULL},
      {"capabilities/media/src/lib/media/VoiceProcessingIo_Apple.cpp", "", 0x9bd69f21afd96a18ULL},
  };
  return pins;
}

} // namespace

TEST(MicHintAudioPathUntouchedTest, TheDeviceAndSessionFunctionsAreTheOnesBeforeTheHint) {
  for (const Pin& pin : Pins()) {
    ASSERT_FALSE(Slice(ReadFile(pin.file), pin.signature).empty()) << pin.file << " " << pin.signature;
    EXPECT_EQ(PinHash(pin.file, pin.signature), pin.hash)
        << pin.file << " " << pin.signature
        << "\nThis pins the audio path for B233 step 1. If you changed these functions on purpose, update the "
           "expected value in the Pins() table of this file (see the header comment).";
  }
}

// The hint is for phones: iOS and Android say yes, every other platform (the default) says no.
TEST(MicHintPlatformTest, OnlyIosAndAndroidShowTheMicrophoneHint) {
  const std::string sig = "bool MicNotOpenHintApplies() {";
  EXPECT_NE(Slice(ReadFile(kIosSession), sig).find("return true;"), std::string::npos);
  EXPECT_NE(Slice(ReadFile(kAndroidSession), sig).find("return true;"), std::string::npos);
  EXPECT_NE(Slice(ReadFile(kDefaultSession), sig).find("return false;"), std::string::npos);
}

// What the engine does to answer "is a microphone wanted but not open" is look: HealthSnapshot calls none of the
// functions that open, close, reopen or configure anything.
TEST(MicHintAudioPathUntouchedTest, TheHealthSnapshotOnlyLooks) {
  const std::string body = Slice(ReadFile(kEngine), "MediaEngineHealth MediaEngine::HealthSnapshot() const {");
  ASSERT_FALSE(body.empty());
  for (const char* forbidden : {"EnsureAudioLeases",
                                "CloseAudioDevicesLocked",
                                "CloseMicBeforeRoute",
                                "RunPendingRouteChange",
                                "TryAcquireVoiceDuplex",
                                "AcquireAudio",
                                "Reopen(",
                                "ActivateForVoip",
                                "Deactivate",
                                "RequestAudioRouteChange",
                                "audio_waiting",
                                "capture_running",
                                "std::thread",
                                ".reset()",
                                "SetMuted",
                                "NoteRouteChanged",
                                "RequestMicPermission",
                                ".store(",
                                ".exchange(",
                                ".fetch_",
                                "Set(MediaThreadStage"}) {
    EXPECT_EQ(body.find(forbidden), std::string::npos) << forbidden;
  }
  // The engine's own opening state is read, never written: only loads.
  EXPECT_NE(body.find("capture_stage.In(MediaThreadStage::OpenDevices)"), std::string::npos);
  EXPECT_NE(body.find("audio_reopening.load("), std::string::npos);
  EXPECT_NE(body.find("audio_reopen_requested.load("), std::string::npos);
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
