#include "domain/media/CallRingtone.h"
#include "domain/media/SdlAudioBootstrap.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace pbr {
namespace {

using namespace std::chrono_literals;

bool WaitFor(const std::atomic<int>& value, int at_least) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (value.load() < at_least && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  return value.load() >= at_least;
}

// Interleaving (1): cancel generation A while it is still inside the device open, redial
// B immediately, then let A's open finish. A must not revive (no second stream), and A's
// exit must not make IsPlaying() report false while B is still playing.
// Uses SDL's "dummy" audio driver only — no real device is opened, nothing is audible.
TEST(CallRingtoneGenerationTest, StoppedGenerationDoesNotReviveAfterFastRedial) {
  SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
  ASSERT_TRUE(EnsureSdlAudioSubsystem());
  const char* driver = SDL_GetCurrentAudioDriver();
  if (!driver || std::strcmp(driver, "dummy") != 0) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    GTEST_SKIP() << "SDL audio already initialized with a real driver; refusing to open it";
  }

  std::atomic<int> opens{0};
  std::atomic<bool> release_first_open{false};
  CallRingtone::SetBeforeOpenHookForTesting([&]() {
    if (opens.fetch_add(1) == 0) {
      while (!release_first_open.load()) {
        std::this_thread::sleep_for(1ms);
      }
    }
  });

  {
    CallRingtone ringback(CallRingtone::Tone::OutgoingRingback);
    ringback.Start(); // A — parks inside the (hooked) device open
    ASSERT_TRUE(WaitFor(opens, 1));
    ringback.Stop();  // caller cancels while A is still opening
    ringback.Start(); // redial: B
    ASSERT_TRUE(WaitFor(opens, 2));
    EXPECT_TRUE(ringback.IsPlaying());

    release_first_open = true; // A's open completes after B's Start()
    std::this_thread::sleep_for(500ms);

    EXPECT_EQ(CallRingtone::PlaybackDeviceHoldersForTesting(), 1)
        << "stopped generation A revived alongside B";
    EXPECT_TRUE(ringback.IsPlaying()) << "A's exit cleared B's IsPlaying()";

    EXPECT_TRUE(ringback.StopAndJoin(2000ms));
    EXPECT_FALSE(ringback.IsPlaying());
    EXPECT_EQ(CallRingtone::PlaybackDeviceHoldersForTesting(), 0);
  }

  CallRingtone::SetBeforeOpenHookForTesting(nullptr);
  SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

} // namespace
} // namespace pbr
