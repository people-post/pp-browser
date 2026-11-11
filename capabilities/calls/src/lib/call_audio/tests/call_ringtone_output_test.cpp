#include "lib/call_audio/CallRingtone.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using namespace std::chrono_literals;

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

struct Speakers {
  std::atomic<int> opens{0};
  std::atomic<int> live{0};
};

class FakeSpeaker final : public IAudioEndpoint {
public:
  explicit FakeSpeaker(Speakers& speakers) : speakers_(speakers) { speakers_.live.fetch_add(1); }
  ~FakeSpeaker() override { speakers_.live.fetch_sub(1); }
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; } // "full": the refill loop only idles
  void Clear() override {}

private:
  Speakers& speakers_;
};

class FakeSpeakerBackend final : public IMediaDeviceBackend {
public:
  explicit FakeSpeakerBackend(Speakers& speakers) : speakers_(speakers) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    speakers_.opens.fetch_add(1);
    return std::make_unique<FakeSpeaker>(speakers_);
  }

private:
  Speakers& speakers_;
};

void PutLe(std::ofstream& out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

/** A short silent 16-bit mono WAV for the incoming ring (CallRingtone reads the file it is given). */
std::string WriteRingWav() {
  const std::string path = ::testing::TempDir() + "call_ringtone_output_test_ring.wav";
  std::ofstream out(path, std::ios::binary);
  const uint32_t rate = 8000;
  const uint32_t data_bytes = 1600;
  out << "RIFF";
  PutLe(out, 36 + data_bytes, 4);
  out << "WAVEfmt ";
  PutLe(out, 16, 4);       // fmt chunk size
  PutLe(out, 1, 2);        // PCM
  PutLe(out, 1, 2);        // mono
  PutLe(out, rate, 4);     // sample rate
  PutLe(out, rate * 2, 4); // byte rate
  PutLe(out, 2, 2);        // block align
  PutLe(out, 16, 2);       // bits per sample
  out << "data";
  PutLe(out, data_bytes, 4);
  for (uint32_t i = 0; i < data_bytes; ++i) {
    out.put(0);
  }
  return path;
}

/** The incoming ring following an injected phone ringer (DEV/PLAN-android-incoming-call-screen.md step 4). */
class CallRingtoneOutputTest : public ::testing::Test {
protected:
  CallRingtoneOutputTest() : devices_(std::make_unique<FakeSpeakerBackend>(speakers_)) {
    CallRingtone::SetRingSoundPath(WriteRingWav());
    ring_ = std::make_unique<CallRingtone>(CallRingtone::Tone::IncomingRing, devices_);
    ring_->SetOutputPorts({.read =
                               [this]() {
                                 ++reads;
                                 return output;
                               },
                           .vibrate = [this](const bool on) { vibrations.push_back(on); }});
  }
  ~CallRingtoneOutputTest() override {
    ring_.reset();
    (void)devices_.Shutdown(2s);
    CallRingtone::SetRingSoundPath({});
  }

  bool SpeakerOpen() {
    return WaitFor([this] { return speakers_.live.load() == 1; });
  }
  bool SpeakerClosed() {
    return WaitFor([this] { return speakers_.live.load() == 0; });
  }

  static constexpr CallRingtone::Output kNormal{.sound = true, .vibrate = false};
  static constexpr CallRingtone::Output kVibrate{.sound = false, .vibrate = true};
  static constexpr CallRingtone::Output kSilent{.sound = false, .vibrate = false};

  Speakers speakers_;
  MediaDeviceArbiter devices_;
  std::unique_ptr<CallRingtone> ring_;
  CallRingtone::Output output = kNormal;
  int reads = 0;
  std::vector<bool> vibrations;
};

TEST_F(CallRingtoneOutputTest, SilentRingsWithoutSoundOrVibration) {
  output = kSilent;
  ring_->Start();
  EXPECT_TRUE(ring_->IsPlaying()) << "the ring is on (its page shows), only quiet";
  EXPECT_FALSE(ring_->IsSounding());
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(speakers_.opens.load(), 0) << "nothing played";
  EXPECT_TRUE(vibrations.empty());
  ring_->Stop();
  EXPECT_FALSE(ring_->IsPlaying());
  EXPECT_TRUE(vibrations.empty());
}

TEST_F(CallRingtoneOutputTest, VibrateModeVibratesUntilTheRingEnds) {
  output = kVibrate;
  ring_->Start();
  EXPECT_FALSE(ring_->IsSounding());
  EXPECT_TRUE(ring_->IsVibrating());
  EXPECT_EQ(vibrations, (std::vector<bool>{true}));
  ring_->Refresh(); // still vibrate: nothing new
  EXPECT_EQ(vibrations, (std::vector<bool>{true}));
  ring_->Stop(); // answered / declined / over
  EXPECT_EQ(vibrations, (std::vector<bool>{true, false}));
  EXPECT_FALSE(ring_->IsVibrating());
  EXPECT_EQ(speakers_.opens.load(), 0);
}

TEST_F(CallRingtoneOutputTest, NormalPlays) {
  ring_->Start();
  EXPECT_TRUE(ring_->IsSounding());
  EXPECT_TRUE(SpeakerOpen());
  EXPECT_TRUE(vibrations.empty());
  ring_->Stop();
  EXPECT_TRUE(SpeakerClosed());
}

TEST_F(CallRingtoneOutputTest, NormalTurnedSilentMidRingStopsTheSound) {
  ring_->Start();
  ASSERT_TRUE(SpeakerOpen());
  output = kSilent;
  ring_->Refresh();
  EXPECT_FALSE(ring_->IsSounding());
  EXPECT_TRUE(SpeakerClosed());
  EXPECT_TRUE(ring_->IsPlaying()) << "still ringing, quietly";
  EXPECT_TRUE(vibrations.empty());
}

TEST_F(CallRingtoneOutputTest, VibrateTurnedNormalStopsTheVibrationAndPlays) {
  output = kVibrate;
  ring_->Start();
  output = kNormal;
  ring_->Refresh();
  EXPECT_EQ(vibrations, (std::vector<bool>{true, false}));
  EXPECT_TRUE(ring_->IsSounding());
  EXPECT_TRUE(SpeakerOpen());
}

TEST_F(CallRingtoneOutputTest, NormalTurnedVibrateStopsTheSoundAndVibrates) {
  ring_->Start();
  ASSERT_TRUE(SpeakerOpen());
  output = kVibrate;
  ring_->Refresh();
  EXPECT_FALSE(ring_->IsSounding());
  EXPECT_TRUE(SpeakerClosed());
  EXPECT_EQ(vibrations, (std::vector<bool>{true}));
  ring_->Stop();
  EXPECT_EQ(vibrations, (std::vector<bool>{true, false}));
}

TEST_F(CallRingtoneOutputTest, RefreshOutsideARingAsksNothing) {
  ring_->Refresh();
  EXPECT_EQ(reads, 0);
  ring_->Start();
  ring_->Stop();
  const int after = reads;
  ring_->Refresh();
  EXPECT_EQ(reads, after);
}

TEST(CallRingtoneOutputDefaultsTest, WithoutPortsTheIncomingRingSounds) {
  Speakers speakers;
  MediaDeviceArbiter devices(std::make_unique<FakeSpeakerBackend>(speakers));
  CallRingtone::SetRingSoundPath(WriteRingWav());
  {
    CallRingtone ring(CallRingtone::Tone::IncomingRing, devices);
    ring.Start();
    EXPECT_TRUE(ring.IsSounding());
    EXPECT_TRUE(WaitFor([&] { return speakers.live.load() == 1; }));
    ring.Stop();
  }
  ASSERT_TRUE(devices.Shutdown(2s));
  CallRingtone::SetRingSoundPath({});
}

// The caller's own ringback ("tuu—") never follows the phone's ringer.
TEST(CallRingtoneOutputDefaultsTest, TheRingbackIgnoresTheRingerPorts) {
  Speakers speakers;
  MediaDeviceArbiter devices(std::make_unique<FakeSpeakerBackend>(speakers));
  int reads = 0;
  bool vibrated = false;
  {
    CallRingtone ringback(CallRingtone::Tone::OutgoingRingback, devices);
    ringback.SetOutputPorts({.read =
                                 [&reads]() {
                                   ++reads;
                                   return CallRingtone::Output{.sound = false, .vibrate = true};
                                 },
                             .vibrate = [&vibrated](bool) { vibrated = true; }});
    ringback.Start();
    ringback.Refresh();
    EXPECT_TRUE(ringback.IsPlaying());
    EXPECT_TRUE(WaitFor([&] { return speakers.live.load() == 1; }));
    EXPECT_TRUE(ringback.StopAndJoin(2000ms));
  }
  EXPECT_EQ(reads, 0);
  EXPECT_FALSE(vibrated);
  ASSERT_TRUE(devices.Shutdown(2s));
}

} // namespace
} // namespace pbr
