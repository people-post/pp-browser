#include "lib/media/AudioSession.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using namespace std::chrono_literals;

// Android's mic around a speaker tap (DEV/PLAN-android-mic-dies-after-speaker.md). The fake models what SDL does on
// the phone: an OS route change while a recording stream is open disconnects it, and from then on every mic open
// "succeeds" but delivers nothing (SDL's dead default recording device) until the audio stack is rebuilt.

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return done();
}

/** The phone's audio as the fake sees it; tests read `events` in order. */
struct Rig {
  std::mutex mu;
  std::vector<std::string> events;
  /** SDL's default recording device is dead: mics open but give nothing. */
  std::atomic<bool> dead{false};
  /** A route change with a mic open kills the device (what the OS + SDL do on Android). */
  std::atomic<bool> route_kills_open_mic{true};
  std::atomic<int> open_mics{0};
  std::atomic<int> resets{0};
  /** What ResetAudio answers, in order; the last one repeats. */
  std::vector<AudioResetResult> reset_results{AudioResetResult::Ok};
  /** An Ok rebuild gives a live device again. */
  std::atomic<bool> reset_fixes{true};
  std::chrono::milliseconds reset_takes{0};
  std::chrono::milliseconds mic_close_takes{0};
  std::atomic<bool> reset_saw_open_stream{false};
  /** The capture thread is held inside a mic read (at most 3 s) — a route change asked meanwhile stays queued. */
  std::atomic<bool> hold_reads{false};
  std::atomic<bool> read_held{false};

  void Note(const std::string& event) {
    std::lock_guard lock(mu);
    events.push_back(event);
  }
  size_t Count(const std::string& event) {
    std::lock_guard lock(mu);
    return static_cast<size_t>(std::count(events.begin(), events.end(), event));
  }
  /** Index of the first `event` at or after `from` (npos when none). */
  size_t Find(const std::string& event, size_t from = 0) {
    std::lock_guard lock(mu);
    for (size_t i = from; i < events.size(); ++i) {
      if (events[i] == event) {
        return i;
      }
    }
    return std::string::npos;
  }
  /** The OS route change a speaker tap makes. */
  void Route(const std::string& name = "route") {
    if (open_mics.load() > 0 && route_kills_open_mic.load()) {
      dead.store(true);
    }
    Note(name);
  }
};

class FakeMic final : public IAudioEndpoint {
public:
  explicit FakeMic(Rig& rig) : rig_(rig) {
    rig_.open_mics.fetch_add(1);
    rig_.Note("mic opened");
  }
  ~FakeMic() override {
    std::this_thread::sleep_for(rig_.mic_close_takes);
    rig_.open_mics.fetch_sub(1);
    rig_.Note("mic closed");
  }
  int Read(void* dst, int bytes) override {
    const auto held_from = std::chrono::steady_clock::now();
    while (rig_.hold_reads.load() && std::chrono::steady_clock::now() - held_from < 3s) {
      rig_.read_held.store(true);
      std::this_thread::sleep_for(2ms);
    }
    rig_.read_held.store(false);
    if (rig_.dead.load()) {
      return 0;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - last_read_ < 20ms) {
      return 0;
    }
    last_read_ = now;
    auto* pcm = static_cast<int16_t*>(dst);
    for (int i = 0; i < bytes / static_cast<int>(sizeof(int16_t)); ++i) {
      pcm[i] = (i % 2 == 0) ? int16_t{8000} : int16_t{-8000};
    }
    return bytes;
  }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 0; }
  void Clear() override {}

private:
  Rig& rig_;
  std::chrono::steady_clock::time_point last_read_{};
};

class QuietSpeaker final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

class RigBackend final : public IMediaDeviceBackend {
public:
  explicit RigBackend(Rig& rig) : rig_(rig) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat&,
                                            const std::function<bool()>&, std::string*) override {
    if (kind == MediaDeviceKind::Mic) {
      return std::make_unique<FakeMic>(rig_);
    }
    return std::make_unique<QuietSpeaker>();
  }
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat&, std::string*) override { return nullptr; }
  bool SupportsAudioReset() const override { return true; }
  AudioResetResult ResetAudio(std::chrono::milliseconds, std::string*) override {
    rig_.Note("reset begin");
    if (rig_.open_mics.load() > 0) {
      rig_.reset_saw_open_stream.store(true);
    }
    std::this_thread::sleep_for(rig_.reset_takes);
    const int n = rig_.resets.fetch_add(1);
    const AudioResetResult result = rig_.reset_results[std::min(static_cast<size_t>(n), rig_.reset_results.size() - 1)];
    if (result == AudioResetResult::Ok && rig_.reset_fixes.load()) {
      rig_.dead.store(false);
    }
    rig_.Note(result == AudioResetResult::Ok ? "reset ok" : "reset missed");
    return result;
  }

private:
  Rig& rig_;
};

/** An engine on the rig whose loud (mic) frames are counted. */
struct Call {
  explicit Call(Rig& rig, bool mic_first = true) : devices(std::make_unique<RigBackend>(rig)), engine(devices) {
    engine.SetCaptureClosesBeforeRouteChangeForTest(mic_first);
    engine.SetEncodeInputObserverForTest([this](const int16_t* pcm, size_t samples) {
      if (std::any_of(pcm, pcm + samples, [](int16_t s) { return s != 0; })) {
        loud.fetch_add(1);
      }
    });
  }
  ~Call() {
    engine.Stop();
    (void)engine.WaitForRetiredSessions(5s);
  }
  void Start(const char* id = "call") {
    auto started = engine.StartDuplex(id, [](const MediaEngine::MediaPacket&) {});
    ASSERT_TRUE(started) << (started ? "" : started.error().message);
  }
  /** Mic audio is going out (again): `more` loud frames after now. */
  bool HeardAgain(int more = 5, std::chrono::milliseconds budget = 3s) {
    const int from = loud.load();
    return WaitFor([&] { return loud.load() >= from + more; }, budget);
  }

  MediaDeviceArbiter devices;
  MediaEngine engine;
  std::atomic<int> loud{0};
};

// A speaker tap with media up: the mic closes, then the route changes, then the mic opens again — the OS never sees
// an open recording stream to disconnect, and no rebuild is needed.
TEST(AudioRouteRecoveryTest, ARouteChangeClosesTheMicFirst) {
  Rig rig;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(call.HeardAgain()) << "the mic never delivered";
  const size_t opened_before = rig.Find("mic opened");
  call.engine.RequestAudioRouteChange([&rig] { rig.Route(); });
  ASSERT_TRUE(WaitFor([&] { return rig.Count("mic opened") >= 2; })) << "the mic was not reopened";
  const size_t closed = rig.Find("mic closed", opened_before);
  const size_t route = rig.Find("route");
  const size_t reopened = rig.Find("mic opened", opened_before + 1);
  ASSERT_NE(route, std::string::npos);
  EXPECT_LT(closed, route) << "the mic was still open when the route changed";
  EXPECT_LT(route, reopened);
  EXPECT_FALSE(rig.dead.load());
  EXPECT_TRUE(call.HeardAgain()) << "the user is heard after the tap";
  EXPECT_EQ(rig.resets.load(), 0) << "the reorder alone keeps the mic: no rebuild";
  EXPECT_EQ(call.engine.AudioRouteChangeState(), IMediaView::AudioRouteChange::None);
}

// The old order (the developer trigger, or a route the app does not control): the OS takes the stream, a reopen gives
// nothing, and one rebuild brings the mic back.
TEST(AudioRouteRecoveryTest, AMicTakenByTheOsComesBackAfterOneRebuild) {
  Rig rig;
  Call call(rig, /*mic_first=*/false);
  call.Start();
  ASSERT_TRUE(call.HeardAgain());
  call.engine.RequestAudioRouteChange([&rig] { rig.Route(); });
  ASSERT_TRUE(WaitFor([&] { return rig.dead.load(); })) << "the fake route did not kill the mic";
  ASSERT_TRUE(WaitFor([&] { return rig.Count("reset ok") == 1; }, 8s)) << "no rebuild after the reopen gave nothing";
  EXPECT_TRUE(call.HeardAgain(5, 3s)) << "the mic is not back after the rebuild";
  EXPECT_FALSE(rig.reset_saw_open_stream.load()) << "rebuilt with a stream still open";
  EXPECT_FALSE(call.engine.HealthSnapshot().capture_lost);
  std::this_thread::sleep_for(2500ms);
  EXPECT_EQ(rig.resets.load(), 1) << "a working mic needs no second rebuild";
}

// A rebuild that ran but brought nothing: never a second one in the same stretch, and the screen is told.
TEST(AudioRouteRecoveryTest, ARebuildThatRanButDidNotHelpIsNotRepeated) {
  Rig rig;
  rig.dead.store(true); // a later call in a process whose device already died
  rig.reset_fixes.store(false);
  Call call(rig);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return rig.resets.load() == 1; }, 8s)) << "no rebuild";
  ASSERT_TRUE(WaitFor([&] { return call.engine.HealthSnapshot().capture_lost; }, 5s)) << "the mic loss is not reported";
  std::this_thread::sleep_for(4500ms);
  EXPECT_EQ(rig.resets.load(), 1) << "rebuilt again with nothing in between";
  EXPECT_TRUE(call.engine.HealthSnapshot().capture_lost);
  // Audio comes back by itself (the OS returned the device): the report clears.
  rig.dead.store(false);
  EXPECT_TRUE(call.HeardAgain(5, 5s));
  EXPECT_TRUE(WaitFor([&] { return !call.engine.HealthSnapshot().capture_lost; }));
}

// A rebuild that did not run (SDL's main thread did not get to it) does not use up the one rebuild: it is tried again.
TEST(AudioRouteRecoveryTest, ARebuildThatDidNotRunIsTriedAgain) {
  Rig rig;
  rig.dead.store(true);
  rig.reset_results = {AudioResetResult::Timeout, AudioResetResult::Ok};
  Call call(rig);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return rig.Count("reset missed") == 1; }, 8s));
  ASSERT_TRUE(WaitFor([&] { return rig.Count("reset ok") == 1; }, 16s)) << "the missed rebuild was never retried";
  EXPECT_TRUE(call.HeardAgain(5, 3s));
  EXPECT_FALSE(call.engine.HealthSnapshot().capture_lost);
}

// A tap while a rebuild runs is not lost: it runs once the rebuild is done, around the fresh mic.
TEST(AudioRouteRecoveryTest, ATapDuringARebuildRunsAfterIt) {
  Rig rig;
  rig.dead.store(true);
  rig.reset_takes = 400ms;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(WaitFor([&] { return rig.Count("reset begin") == 1; }, 8s));
  call.engine.RequestAudioRouteChange([&rig] { rig.Route("route during rebuild"); });
  ASSERT_TRUE(WaitFor([&] { return rig.Count("route during rebuild") == 1; }, 3s)) << "the tap was dropped";
  EXPECT_LT(rig.Find("reset ok"), rig.Find("route during rebuild"));
  EXPECT_FALSE(rig.dead.load()) << "the route changed with the fresh mic open";
  EXPECT_TRUE(call.HeardAgain());
}

// Nothing captures yet (or any more): the route changes at once — there is no recording stream to protect.
TEST(AudioRouteRecoveryTest, ATapBeforeCaptureRunsAtOnce) {
  Rig rig;
  Call call(rig);
  call.engine.RequestAudioRouteChange([&rig] { rig.Route(); });
  EXPECT_EQ(rig.Count("route"), 1u);
  EXPECT_EQ(call.engine.AudioRouteChangeState(), IMediaView::AudioRouteChange::None);
}

// Tapped, then hung up before the engine got to it: the route change is dropped with the call — it never runs after
// the session (nor after the OS route went back), so the next call starts on the earpiece.
TEST(AudioRouteRecoveryTest, ATapLeftWhenTheCallEndsIsDropped) {
  Rig rig;
  Call call(rig);
  call.Start("first");
  ASSERT_TRUE(call.HeardAgain());
  rig.hold_reads.store(true); // the capture thread is busy: the change waits in the queue
  ASSERT_TRUE(WaitFor([&] { return rig.read_held.load(); }));
  call.engine.RequestAudioRouteChange([&rig] { rig.Route(); });
  EXPECT_EQ(call.engine.AudioRouteChangeState(), IMediaView::AudioRouteChange::Queued);
  call.engine.Stop();
  rig.hold_reads.store(false);
  ASSERT_TRUE(call.engine.WaitForRetiredSessions(3s));
  call.Start("second");
  ASSERT_TRUE(call.HeardAgain());
  EXPECT_EQ(rig.Count("route"), 0u) << "the old call's speaker tap ran";
  EXPECT_EQ(call.engine.AudioRouteChangeState(), IMediaView::AudioRouteChange::None);
}

// Two taps before the engine gets to them: only the latest runs, with one close / reopen of the mic.
TEST(AudioRouteRecoveryTest, ADoubleTapRunsOnlyTheLatest) {
  Rig rig;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(call.HeardAgain());
  rig.hold_reads.store(true);
  ASSERT_TRUE(WaitFor([&] { return rig.read_held.load(); }));
  call.engine.RequestAudioRouteChange([&rig] { rig.Route("route on"); });
  call.engine.RequestAudioRouteChange([&rig] { rig.Route("route off"); });
  const size_t opens = rig.Count("mic opened");
  rig.hold_reads.store(false);
  ASSERT_TRUE(WaitFor([&] { return rig.Count("route off") == 1; }));
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(rig.Count("route on"), 0u);
  EXPECT_EQ(rig.Count("route off"), 1u);
  EXPECT_EQ(rig.Count("mic opened"), opens + 1) << "one close / reopen for the two taps";
  EXPECT_FALSE(rig.dead.load());
  EXPECT_TRUE(call.HeardAgain());
}

// A tap while the OS has the session (Android: the call's audio focus is lost) still changes the route at once — on
// Android that tap is what asks for the focus again — with the mic closed first; the mic reopens when the session is
// back (opens fail while the OS has it).
TEST(AudioRouteRecoveryTest, ATapWhileTheOsHasTheSessionReopensAtItsEnd) {
  AudioSession::ClearInterruption();
  Rig rig;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(call.HeardAgain());
  const size_t opens = rig.Count("mic opened");
  AudioSession::NotifyInterruptionBegan();
  call.engine.RequestAudioRouteChange([&rig] { rig.Route(); });
  ASSERT_TRUE(WaitFor([&] { return rig.Count("route") == 1; })) << "the route waited for the session";
  EXPECT_FALSE(rig.dead.load()) << "the mic was open when the route changed";
  std::this_thread::sleep_for(200ms);
  EXPECT_EQ(rig.Count("mic opened"), opens) << "a mic opened while the OS had the session";
  AudioSession::NotifyInterruptionEnded();
  EXPECT_TRUE(WaitFor([&] { return rig.Count("mic opened") > opens; })) << "not reopened at the session's end";
  EXPECT_TRUE(call.HeardAgain());
  AudioSession::ClearInterruption();
}

// At a call's end the mic is closed before the OS route goes back (Deactivate runs after the retire thread's drain).
TEST(AudioRouteRecoveryTest, AtACallsEndTheMicClosesBeforeTheRouteGoesBack) {
  Rig rig;
  rig.mic_close_takes = 300ms;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(call.HeardAgain());
  call.engine.Stop();
  ASSERT_TRUE(call.engine.WaitForRetiredSessions(3s));
  EXPECT_EQ(rig.Count("mic closed"), 1u) << "the retire finished before the mic was closed";
}

// A backend that closes slowly does not hold the hang-up: Stop returns at once, and the retire gives up waiting
// after its budget (1 s) — the route goes back anyway and the next call's rebuild would recover the mic.
TEST(AudioRouteRecoveryTest, ASlowCloseDoesNotHoldTheHangUp) {
  Rig rig;
  rig.mic_close_takes = 2500ms;
  Call call(rig);
  call.Start();
  ASSERT_TRUE(call.HeardAgain());
  const auto from = std::chrono::steady_clock::now();
  call.engine.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - from, 200ms) << "Stop waited for the device";
  EXPECT_TRUE(call.engine.WaitForRetiredSessions(2000ms)) << "the retire waited past its budget";
  EXPECT_EQ(rig.Count("mic closed"), 0u) << "(the close is still running)";
  ASSERT_TRUE(WaitFor([&] { return rig.Count("mic closed") == 1u; }, 3s));
}

// The arbiter refuses a rebuild while someone else holds an audio device (a ringtone's speaker): it is not run.
TEST(AudioRouteRecoveryTest, NoRebuildWhileAnotherHolderHasTheSpeaker) {
  Rig rig;
  MediaDeviceArbiter devices(std::make_unique<RigBackend>(rig));
  AudioLeaseRequest request;
  request.kind = MediaDeviceKind::Speaker;
  request.holder = "ringtone";
  auto lease = devices.AcquireAudio(request);
  ASSERT_TRUE(lease);
  std::string why;
  EXPECT_EQ(devices.ResetAudio(500ms, &why), AudioResetResult::Busy);
  EXPECT_EQ(rig.resets.load(), 0);
  lease->reset();
  EXPECT_EQ(devices.ResetAudio(500ms, &why), AudioResetResult::Ok);
  EXPECT_EQ(rig.resets.load(), 1);
}

// An open asked for while a rebuild runs waits for it (FIFO on the audio device thread): no device opens mid-rebuild.
TEST(AudioRouteRecoveryTest, AnOpenDuringARebuildWaitsForIt) {
  Rig rig;
  rig.reset_takes = 300ms;
  MediaDeviceArbiter devices(std::make_unique<RigBackend>(rig));
  std::thread rebuild([&] {
    std::string why;
    (void)devices.ResetAudio(1000ms, &why);
  });
  ASSERT_TRUE(WaitFor([&] { return rig.Count("reset begin") == 1; }));
  AudioLeaseRequest request;
  request.kind = MediaDeviceKind::Mic;
  request.holder = "call";
  auto lease = devices.AcquireAudio(request);
  rebuild.join();
  ASSERT_TRUE(lease);
  EXPECT_LT(rig.Find("reset ok"), rig.Find("mic opened")) << "a mic opened during the rebuild";
}

// Platforms without a rebuild (every backend but Android's SDL one) are never asked for one.
TEST(AudioRouteRecoveryTest, NoRebuildWhereTheBackendHasNone) {
  MediaDeviceArbiter devices(CreateNullMediaDeviceBackend());
  EXPECT_FALSE(devices.CanResetAudio());
  std::string why;
  EXPECT_EQ(devices.ResetAudio(100ms, &why), AudioResetResult::Unsupported);
}

} // namespace
} // namespace pbr
