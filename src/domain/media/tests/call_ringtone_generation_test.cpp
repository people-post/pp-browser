#include "domain/media/CallRingtone.h"
#include "domain/media/MediaDeviceArbiter.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

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

/** Speakers that count themselves while open; the first open parks until released. */
struct SpeakerCounter {
  std::atomic<int> opens{0};
  std::atomic<int> live{0};
  std::atomic<bool> release_first_open{false};
};

class CountedSpeaker final : public IAudioEndpoint {
public:
  explicit CountedSpeaker(SpeakerCounter& counter) : counter_(counter) { counter_.live.fetch_add(1); }
  ~CountedSpeaker() override { counter_.live.fetch_sub(1); }
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }  // "full": the refill loop only idles
  void Clear() override {}

private:
  SpeakerCounter& counter_;
};

class ParkingBackend final : public IMediaDeviceBackend {
public:
  explicit ParkingBackend(SpeakerCounter& counter) : counter_(counter) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind, const AudioDeviceFormat&, const std::function<bool()>&,
                                            std::string*) override {
    if (counter_.opens.fetch_add(1) == 0) {
      while (!counter_.release_first_open.load()) {
        std::this_thread::sleep_for(1ms);
      }
    }
    return std::make_unique<CountedSpeaker>(counter_);
  }

private:
  SpeakerCounter& counter_;
};

// Cancel generation A while its speaker is still opening, redial B at once, then let A's open
// finish. A must not revive (one speaker open once settled), and A's exit must not make
// IsPlaying() report false while B plays. Opens queue on the arbiter's one device thread, so B's
// open runs after A's.
TEST(CallRingtoneGenerationTest, StoppedGenerationDoesNotReviveAfterFastRedial) {
  SpeakerCounter counter;
  MediaDeviceArbiter devices(std::make_unique<ParkingBackend>(counter));
  {
    CallRingtone ringback(CallRingtone::Tone::OutgoingRingback, devices);
    ringback.Start();  // A — parks inside the device open
    ASSERT_TRUE(WaitFor([&] { return counter.opens.load() >= 1; }));
    ringback.Stop();   // caller cancels while A is still opening
    ringback.Start();  // redial: B
    EXPECT_TRUE(ringback.IsPlaying());

    counter.release_first_open = true;  // A's open completes after B's Start()
    ASSERT_TRUE(WaitFor([&] { return counter.opens.load() >= 2; })) << "B never opened";
    EXPECT_TRUE(WaitFor([&] { return counter.live.load() == 1; })) << "stopped generation A revived alongside B";
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(counter.live.load(), 1);
    EXPECT_TRUE(ringback.IsPlaying()) << "A's exit cleared B's IsPlaying()";

    EXPECT_TRUE(ringback.StopAndJoin(2000ms));
    EXPECT_FALSE(ringback.IsPlaying());
    EXPECT_FALSE(CallRingtone::RingbackSessionHeld()) << "every generation released its session";
  }
  ASSERT_TRUE(devices.Shutdown(2s));
  EXPECT_EQ(counter.live.load(), 0);
}

// Media taking over (release = false) still ends the ringback's session hold, so the call engine's
// wait for it returns at once.
TEST(CallRingtoneGenerationTest, HandingTheSessionToMediaEndsTheRingbackHold) {
  SpeakerCounter counter;
  counter.release_first_open = true;
  MediaDeviceArbiter devices(std::make_unique<ParkingBackend>(counter));
  CallRingtone ringback(CallRingtone::Tone::OutgoingRingback, devices);
  ringback.Start();
  ASSERT_TRUE(WaitFor([] { return CallRingtone::RingbackSessionHeld(); }));
  ringback.SetReleaseSessionOnStop(false);
  EXPECT_TRUE(ringback.StopAndJoin(2000ms));
  EXPECT_FALSE(CallRingtone::RingbackSessionHeld());
}

} // namespace
} // namespace pbr
