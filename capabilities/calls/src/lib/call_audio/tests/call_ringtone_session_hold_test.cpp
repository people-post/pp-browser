#include "lib/call_audio/CallRingtone.h"
#include "lib/media/AudioSession.h"
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

namespace pbr {
namespace {

using namespace std::chrono_literals;

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return done();
}

/** Speakers whose close (the endpoint's destruction, on the arbiter's device thread) can be held back. */
struct Speakers {
  std::atomic<int> live{0};
  std::atomic<bool> park_close{false};
  std::atomic<bool> no_device{false};
};

class ParkedCloseSpeaker final : public IAudioEndpoint {
public:
  explicit ParkedCloseSpeaker(Speakers& speakers) : speakers_(speakers) { speakers_.live.fetch_add(1); }
  ~ParkedCloseSpeaker() override {
    while (speakers_.park_close.load()) {
      std::this_thread::sleep_for(1ms);
    }
    speakers_.live.fetch_sub(1);
  }
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; } // "full": the refill loop only idles
  void Clear() override {}

private:
  Speakers& speakers_;
};

class ParkedCloseBackend final : public IMediaDeviceBackend {
public:
  explicit ParkedCloseBackend(Speakers& speakers) : speakers_(speakers) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string* error) override {
    if (speakers_.no_device.load()) {
      if (error) {
        *error = "no speaker";
      }
      return nullptr;
    }
    return std::make_unique<ParkedCloseSpeaker>(speakers_);
  }

private:
  Speakers& speakers_;
};

void PutLe(std::ofstream& out, const uint32_t value, const int bytes) {
  for (int i = 0; i < bytes; ++i) {
    out.put(static_cast<char>((value >> (8 * i)) & 0xFF));
  }
}

/** A short silent 16-bit mono WAV for the incoming ring. */
std::string WriteRingWav() {
  const std::string path = ::testing::TempDir() + "call_ringtone_session_hold_test_ring.wav";
  std::ofstream out(path, std::ios::binary);
  const uint32_t rate = 8000;
  const uint32_t data_bytes = 1600;
  out << "RIFF";
  PutLe(out, 36 + data_bytes, 4);
  out << "WAVEfmt ";
  PutLe(out, 16, 4);
  PutLe(out, 1, 2);
  PutLe(out, 1, 2);
  PutLe(out, rate, 4);
  PutLe(out, rate * 2, 4);
  PutLe(out, 2, 2);
  PutLe(out, 16, 2);
  out << "data";
  PutLe(out, data_bytes, 4);
  for (uint32_t i = 0; i < data_bytes; ++i) {
    out.put(0);
  }
  return path;
}

/**
 * B230: an incoming ring stopped by an answer holds the audio session until its speaker stream is closed, so call
 * media (AudioSession::WaitUntilToneHoldsReleased before ActivateForVoip and the voice-processing unit) starts after
 * that close. Hold forced on (SetHoldSessionUntilClosed): the host default is the platform's (iOS only).
 */
class CallRingtoneSessionHoldTest : public ::testing::Test {
protected:
  CallRingtoneSessionHoldTest() : devices_(std::make_unique<ParkedCloseBackend>(speakers_)) {
    CallRingtone::SetRingSoundPath(WriteRingWav());
  }
  ~CallRingtoneSessionHoldTest() override {
    speakers_.park_close = false;
    ring_.reset();
    (void)devices_.Shutdown(3s);
    CallRingtone::SetRingSoundPath({});
  }

  CallRingtone& MakeRing(const bool hold) {
    ring_ = std::make_unique<CallRingtone>(CallRingtone::Tone::IncomingRing, devices_);
    ring_->SetHoldSessionUntilClosed(hold);
    return *ring_;
  }

  Speakers speakers_;
  MediaDeviceArbiter devices_;
  std::unique_ptr<CallRingtone> ring_;
};

TEST_F(CallRingtoneSessionHoldTest, AnsweredRingHoldsTheSessionUntilItsStreamIsClosed) {
  CallRingtone& ring = MakeRing(/*hold=*/true);
  ring.Start();
  ASSERT_TRUE(WaitFor([&] { return speakers_.live.load() == 1; }));
  EXPECT_FALSE(AudioSession::ToneHoldsSession()) << "ringing alone holds nothing (a call over it is not delayed)";

  speakers_.park_close = true; // the close is slow
  ring.Stop();                 // the answer
  EXPECT_TRUE(AudioSession::ToneHoldsSession()) << "the hold is taken by the stop itself, before media can start";
  EXPECT_FALSE(AudioSession::WaitUntilToneHoldsReleased(100ms)) << "media start waits while the stream is open";
  EXPECT_EQ(speakers_.live.load(), 1);

  speakers_.park_close = false; // the close runs
  EXPECT_TRUE(AudioSession::WaitUntilToneHoldsReleased(3s));
  EXPECT_EQ(speakers_.live.load(), 0) << "the hold ended only after the stream was closed";
}

TEST_F(CallRingtoneSessionHoldTest, MediaStartGivesUpWaitingAtItsBoundAndTheHoldStillEnds) {
  CallRingtone& ring = MakeRing(/*hold=*/true);
  ring.Start();
  ASSERT_TRUE(WaitFor([&] { return speakers_.live.load() == 1; }));
  speakers_.park_close = true; // a close that does not finish
  ring.Stop();

  const auto from = std::chrono::steady_clock::now();
  EXPECT_FALSE(AudioSession::WaitUntilToneHoldsReleased(150ms)) << "timed out: MediaEngine logs and activates anyway";
  const auto waited = std::chrono::steady_clock::now() - from;
  EXPECT_GE(waited, 150ms);
  EXPECT_LT(waited, 1s) << "bounded: media start is never stuck behind the ring";

  // The ring itself gives up on the close after kRingCloseWait: no hold is left for the next call.
  EXPECT_TRUE(WaitFor([] { return !AudioSession::ToneHoldsSession(); }, CallRingtone::kRingCloseWait + 1s));
  speakers_.park_close = false;
}

TEST_F(CallRingtoneSessionHoldTest, WithoutTheHoldAStopHoldsNothing) {
  CallRingtone& ring = MakeRing(/*hold=*/false); // every platform but iOS
  ring.Start();
  ASSERT_TRUE(WaitFor([&] { return speakers_.live.load() == 1; }));
  speakers_.park_close = true;
  ring.Stop();
  EXPECT_FALSE(AudioSession::ToneHoldsSession());
  EXPECT_TRUE(AudioSession::WaitUntilToneHoldsReleased(0ms));
  speakers_.park_close = false;
  EXPECT_TRUE(WaitFor([&] { return speakers_.live.load() == 0; }));
}

TEST_F(CallRingtoneSessionHoldTest, ARingWithoutASpeakerLeavesNoHold) {
  speakers_.no_device = true;
  CallRingtone& ring = MakeRing(/*hold=*/true);
  ring.Start();
  std::this_thread::sleep_for(50ms); // the worker found no speaker and finished
  ring.Stop();
  EXPECT_TRUE(WaitFor([] { return !AudioSession::ToneHoldsSession(); }));
}

TEST_F(CallRingtoneSessionHoldTest, StoppingTwiceTakesOneHold) {
  CallRingtone& ring = MakeRing(/*hold=*/true);
  ring.Start();
  ASSERT_TRUE(WaitFor([&] { return speakers_.live.load() == 1; }));
  speakers_.park_close = true;
  ring.Stop();
  ring.Stop();
  EXPECT_TRUE(AudioSession::ToneHoldsSession());
  speakers_.park_close = false;
  EXPECT_TRUE(AudioSession::WaitUntilToneHoldsReleased(3s)) << "a second stop left a hold nobody ends";
}

} // namespace
} // namespace pbr
