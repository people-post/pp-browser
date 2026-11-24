#include "lib/media/AudioSession.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaEngine.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using namespace std::chrono_literals;

class QuietEndpoint final : public IAudioEndpoint {
public:
  int Read(void*, int) override { return 0; }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 1 << 20; }
  void Clear() override {}
};

/**
 * The OS wedging the capture side while it has the session: once set, every mic read blocks until let go. One iOS
 * shape of it: SDL's recording stream (the fallback when voice processing will not start in the background) is read
 * under a lock its device thread holds while its audio queue is stuck.
 */
struct CaptureWedge {
  std::mutex mu;
  std::condition_variable cv;
  bool wedged = false;
  bool let_go = false;
  std::atomic<int> blocked_reads{0};

  void Wedge() {
    std::lock_guard lock(mu);
    wedged = true;
  }
  void LetGo() {
    {
      std::lock_guard lock(mu);
      let_go = true;
    }
    cv.notify_all();
  }
  void BlockIfWedged() {
    std::unique_lock lock(mu);
    if (!wedged || let_go) {
      return;
    }
    blocked_reads.fetch_add(1);
    cv.wait(lock, [this] { return let_go; });
  }
};

class WedgingMic final : public IAudioEndpoint {
public:
  explicit WedgingMic(CaptureWedge& wedge) : wedge_(wedge) {}
  int Read(void*, int) override {
    wedge_.BlockIfWedged();
    return 0;
  }
  bool Write(const void*, int) override { return false; }
  int Queued() const override { return 0; }
  void Clear() override {}

private:
  CaptureWedge& wedge_;
};

/** Separate mic and speaker (no voice-processing pair), as on iOS once the pair would not open. */
class WedgingBackend final : public IMediaDeviceBackend {
public:
  explicit WedgingBackend(CaptureWedge& wedge) : wedge_(wedge) {}
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat&,
                                            const std::function<bool()>&, std::string*) override {
    if (kind == MediaDeviceKind::Mic) {
      return std::make_unique<WedgingMic>(wedge_);
    }
    return std::make_unique<QuietEndpoint>();
  }

private:
  CaptureWedge& wedge_;
};

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(5ms);
  }
  return done();
}

bool Holds(const MediaDeviceArbiter& devices, MediaDeviceKind kind, const std::string& holder) {
  const std::vector<std::string> holders = devices.Holders(kind);
  return std::find(holders.begin(), holders.end(), holder) != holders.end();
}

std::string HoldersText(const MediaDeviceArbiter& devices, MediaDeviceKind kind) {
  std::string text;
  for (const std::string& holder : devices.Holders(kind)) {
    text += (text.empty() ? "" : ",") + holder;
  }
  return text.empty() ? "nobody" : text;
}

// iPhone, 2026-11-19: a call placed in the app (not on CallKit) lost its audio to a video played in another app; the
// call's audio never came back, and after a redial the other side still could not hear this phone while this phone
// heard them, until the app was force-quit. A call that has ended must not keep the mic: whatever wedged its media
// threads, the next call gets it (the speaker is shared, so it never showed there).
TEST(MediaEngineRedialTest, ARedialGetsTheMicAfterTheEndedCallsCaptureWedgedInAnInterruption) {
  AudioSession::ClearInterruption();
  CaptureWedge wedge;
  MediaDeviceArbiter devices(std::make_unique<WedgingBackend>(wedge));
  MediaEngine engine(devices);

  auto first = engine.StartDuplex("first-call", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(first) << (first ? "" : first.error().message);
  ASSERT_TRUE(WaitFor([&] { return Holds(devices, MediaDeviceKind::Mic, "first-call"); }))
      << "the first call never took the mic";

  AudioSession::NotifyInterruptionBegan(); // another app's video takes the audio session
  wedge.Wedge();
  ASSERT_TRUE(WaitFor([&] { return wedge.blocked_reads.load() > 0; })) << "the capture thread never read the mic";

  engine.Stop();                           // hang up
  AudioSession::NotifyInterruptionEnded(); // the app is back in front: the session is resumed

  auto redial = engine.StartDuplex("redial", [](const MediaEngine::MediaPacket&) {});
  ASSERT_TRUE(redial) << (redial ? "" : redial.error().message);
  EXPECT_TRUE(WaitFor([&] { return Holds(devices, MediaDeviceKind::Speaker, "redial"); }))
      << "the redial has no speaker";
  // Past the engine's mic handover wait (1.5 s) and a few of its device retries (every 100 ms).
  EXPECT_TRUE(WaitFor([&] { return Holds(devices, MediaDeviceKind::Mic, "redial"); }, 5s))
      << "the redial has no mic; held by " << HoldersText(devices, MediaDeviceKind::Mic);

  // The ended call's thread gets out at last: its late cleanup closes its own endpoints and takes nothing back.
  wedge.LetGo();
  EXPECT_TRUE(engine.WaitForRetiredSessions(5s)) << "the ended call never finished cleaning up";
  EXPECT_TRUE(Holds(devices, MediaDeviceKind::Mic, "redial")) << "the redial lost its mic to the ended call's cleanup";
  EXPECT_FALSE(Holds(devices, MediaDeviceKind::Mic, "first-call"));

  engine.Stop();
  EXPECT_TRUE(engine.WaitForRetiredSessions(5s)) << "a session never finished cleaning up";
  AudioSession::ClearInterruption();
}

} // namespace
} // namespace pbr
