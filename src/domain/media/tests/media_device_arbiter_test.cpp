#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/VideoCodecUnavailable.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

/** Records every open / close with its thread; flags any overlap between two device operations. */
struct DeviceLog {
  std::mutex mu;
  std::vector<std::string> events;
  std::set<std::thread::id> threads;
  std::atomic<int> in_flight{0};
  std::atomic<bool> overlapped{false};
  std::atomic<int> writes{0};

  void Enter(const std::string& event) {
    if (in_flight.fetch_add(1) != 0) {
      overlapped = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // widen any race window
    std::lock_guard lock(mu);
    events.push_back(event);
    threads.insert(std::this_thread::get_id());
  }
  void Leave() { in_flight.fetch_sub(1); }
  std::vector<std::string> Events() {
    std::lock_guard lock(mu);
    return events;
  }
};

class FakeEndpoint final : public IAudioEndpoint {
public:
  FakeEndpoint(DeviceLog& log, std::string name) : log_(log), name_(std::move(name)) {}
  ~FakeEndpoint() override {
    log_.Enter("close " + name_);
    log_.Leave();
  }
  /** Paced like a device: one 20 ms chunk per period, nothing in between. */
  int Read(void* dst, int bytes) override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_read_ < std::chrono::milliseconds(20)) {
      return 0;
    }
    last_read_ = now;
    std::memset(dst, 0, static_cast<size_t>(bytes));
    return bytes;
  }
  bool Write(const void* /*src*/, int /*bytes*/) override {
    log_.writes.fetch_add(1);
    return true;
  }
  int Queued() const override { return 0; }
  void Clear() override {}

private:
  DeviceLog& log_;
  std::string name_;
  std::chrono::steady_clock::time_point last_read_{};
};

/** 64×36 RGBA frames, one per 30 ms. */
class FakeCamera final : public ICameraEndpoint {
public:
  explicit FakeCamera(DeviceLog& log) : log_(log) {}
  ~FakeCamera() override {
    log_.Enter("close camera");
    log_.Leave();
  }
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36}; }
  std::optional<VideoFrameRgba> NextFrame() override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_ < std::chrono::milliseconds(30)) {
      return std::nullopt;
    }
    last_ = now;
    VideoFrameRgba frame;
    frame.width = 64;
    frame.height = 36;
    frame.rgba.assign(64u * 36u * 4u, 0x80);
    return frame;
  }

private:
  DeviceLog& log_;
  std::chrono::steady_clock::time_point last_{};
};

class FakeBackend final : public IMediaDeviceBackend {
public:
  explicit FakeBackend(DeviceLog& log) : log_(log) {}
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& format, std::string* error) override {
    last_display_rotation = format.display_rotation_deg;
    std::this_thread::sleep_for(camera_open_delay.load());
    log_.Enter("open camera");
    log_.Leave();
    if (!camera_present) {
      *error = "No camera: none attached";
      return nullptr;
    }
    return std::make_unique<FakeCamera>(log_);
  }
  std::atomic<bool> camera_present{true};
  std::atomic<std::chrono::milliseconds> camera_open_delay{std::chrono::milliseconds(0)};
  std::atomic<int> last_display_rotation{-1};
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat& /*format*/,
                                       const std::function<bool()>& /*still_wanted*/,
                                       std::string* error) override {
    const std::string name = MediaDeviceKindName(kind);
    log_.Enter("open " + name);
    log_.Leave();
    if (!present) {
      *error = "absent";
      return nullptr;
    }
    return std::make_unique<FakeEndpoint>(log_, name);
  }
  std::atomic<bool> present{true};

private:
  DeviceLog& log_;
};

AudioLeaseRequest Request(MediaDeviceKind kind, const std::string& holder) {
  AudioLeaseRequest r;
  r.kind = kind;
  r.holder = holder;
  return r;
}

class MediaDeviceArbiterTest : public ::testing::Test {
protected:
  MediaDeviceArbiterTest() {
    auto backend = std::make_unique<FakeBackend>(log_);
    backend_ = backend.get();
    arbiter_ = std::make_unique<MediaDeviceArbiter>(std::move(backend));
  }
  void Settle() { ASSERT_TRUE(arbiter_->Shutdown(std::chrono::seconds(2))); }

  DeviceLog log_;
  FakeBackend* backend_ = nullptr;
  std::unique_ptr<MediaDeviceArbiter> arbiter_;
};

TEST_F(MediaDeviceArbiterTest, MicIsExclusiveAndRefusalNamesHolder) {
  auto first = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "session-a"));
  ASSERT_TRUE(first);
  EXPECT_TRUE((*first)->HasDevice());
  auto second = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "session-b"));
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().message, "mic held by session-a");
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic), std::vector<std::string>{"session-a"});
}

TEST_F(MediaDeviceArbiterTest, SpeakerIsSharedSoRingingMixesOverACall) {
  auto call = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "call"));
  auto ring = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "ringtone"));
  ASSERT_TRUE(call);
  ASSERT_TRUE(ring);
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Speaker).size(), 2u);
}

// A session rebuild releases and immediately re-takes the mic: not refused, and the old endpoint
// closes before the new one opens (FIFO on the device thread).
TEST_F(MediaDeviceArbiterTest, ReleaseThenReacquireClosesBeforeOpening) {
  auto first = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "a"));
  ASSERT_TRUE(first);
  first->reset();
  auto again = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "a"));
  ASSERT_TRUE(again) << again.error().message;
  again->reset();
  Settle();
  EXPECT_EQ(log_.Events(), (std::vector<std::string>{"open mic", "close mic", "open mic", "close mic"}));
}

// The ringtone ↔ call-media hang: two holders opening / closing from their own threads must never
// overlap at the OS, and all of it runs on the one device thread.
TEST_F(MediaDeviceArbiterTest, OpensAndClosesFromManyThreadsRunSeriallyOnOneThread) {
  std::vector<std::thread> holders;
  for (int t = 0; t < 4; ++t) {
    holders.emplace_back([this, t]() {
      for (int i = 0; i < 10; ++i) {
        auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "h" + std::to_string(t)));
        ASSERT_TRUE(lease);
        (void)(*lease)->Write("x", 1);
      }
    });
  }
  for (auto& h : holders) {
    h.join();
  }
  Settle();
  EXPECT_FALSE(log_.overlapped.load());
  EXPECT_EQ(log_.threads.size(), 1u);
  EXPECT_EQ(log_.threads.count(std::this_thread::get_id()), 0u) << "not on a caller thread";
  EXPECT_EQ(log_.Events().size(), 80u);
}

TEST_F(MediaDeviceArbiterTest, AbsentDeviceStillGrantsTheLease) {
  backend_->present = false;
  auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "headless"));
  ASSERT_TRUE(lease);
  EXPECT_FALSE((*lease)->HasDevice());
  EXPECT_EQ((*lease)->OpenError(), "absent");
  char buf[8];
  EXPECT_EQ((*lease)->Read(buf, sizeof(buf)), 0);
  EXPECT_FALSE((*lease)->Write(buf, sizeof(buf)));
  EXPECT_FALSE(arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "other"))) << "still held";
}

TEST_F(MediaDeviceArbiterTest, ReopenSwapsTheEndpointAndCanRecoverAMissingDevice) {
  backend_->present = false;
  auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "a"));
  ASSERT_TRUE(lease);
  EXPECT_FALSE((*lease)->HasDevice());
  backend_->present = true;
  ASSERT_TRUE((*lease)->Reopen());
  EXPECT_TRUE((*lease)->HasDevice());
  ASSERT_TRUE((*lease)->Reopen());
  lease->reset();
  Settle();
  EXPECT_EQ(log_.Events(), (std::vector<std::string>{"open mic", "open mic", "close mic", "open mic", "close mic"}));
}

// Writers keep going while the holder reopens: they see "no device", never a closed endpoint.
TEST_F(MediaDeviceArbiterTest, IoDuringReopenNeverTouchesAClosedEndpoint) {
  auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "a"));
  ASSERT_TRUE(lease);
  std::atomic<bool> run{true};
  std::thread writer([&]() {
    while (run.load()) {
      (void)(*lease)->Write("x", 1);
    }
  });
  for (int i = 0; i < 20; ++i) {
    ASSERT_TRUE((*lease)->Reopen());
  }
  run = false;
  writer.join();
  EXPECT_GT(log_.writes.load(), 0);
  EXPECT_FALSE(log_.overlapped.load());
}

TEST_F(MediaDeviceArbiterTest, LeaseOutlivingTheArbiterClosesInline) {
  auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "late"));
  ASSERT_TRUE(lease);
  arbiter_.reset();
  lease->reset();
  EXPECT_EQ(log_.Events().back(), "close speaker");
}

TEST_F(MediaDeviceArbiterTest, ShutdownRunsQueuedClosesThenOpensInline) {
  auto lease = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "a"));
  ASSERT_TRUE(lease);
  lease->reset();
  Settle();
  EXPECT_EQ(log_.Events().back(), "close speaker");
  auto after = arbiter_->AcquireAudio(Request(MediaDeviceKind::Speaker, "b"));
  ASSERT_TRUE(after);
  EXPECT_TRUE((*after)->HasDevice());
}

TEST_F(MediaDeviceArbiterTest, CameraIsExclusiveAndFramesFlowThroughTheLease) {
  CameraLeaseRequest request;
  request.holder = "call:1";
  auto camera = arbiter_->AcquireCamera(request);
  ASSERT_TRUE(camera) << camera.error().message;
  EXPECT_EQ((*camera)->Geometry().encode_width, 64);
  request.holder = "call:2";
  auto second = arbiter_->AcquireCamera(request);
  ASSERT_FALSE(second);
  EXPECT_EQ(second.error().message, "camera held by call:1");
  std::optional<VideoFrameRgba> frame;
  for (int i = 0; i < 20 && !frame; ++i) {
    frame = (*camera)->NextFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(frame);
  EXPECT_EQ(frame->width, 64);
  camera->reset();
  Settle();
  EXPECT_EQ(log_.Events().back(), "close camera");
}

TEST_F(MediaDeviceArbiterTest, MissingCameraIsRefusedAndFreesTheSlot) {
  backend_->camera_present = false;
  CameraLeaseRequest request;
  request.holder = "call:1";
  auto camera = arbiter_->AcquireCamera(request);
  ASSERT_FALSE(camera);
  EXPECT_EQ(camera.error().message, "No camera: none attached");
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
  backend_->camera_present = true;
  EXPECT_TRUE(arbiter_->AcquireCamera(request));
}

// --- engine takes the leases its session spec asks for -----------------------------------------

class EngineDeviceLeaseTest : public MediaDeviceArbiterTest {
protected:
  /** Engine on the fake devices, with a stub codec: these tests are about devices, not the host's HW. */
  std::unique_ptr<CallMediaEngine> MakeEngine() {
    auto engine = std::make_unique<CallMediaEngine>(*arbiter_);
    engine->SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test stub"); });
    return engine;
  }
  bool WaitHolders(MediaDeviceKind kind, size_t n) {
    for (int i = 0; i < 400; ++i) {
      if (arbiter_->Holders(kind).size() == n) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }
  static CallMediaEngine::SfuSendFn NoopSend() {
    return [](const CallMediaEngine::SfuPacket&) {};
  }
};

TEST_F(EngineDeviceLeaseTest, DuplexHoldsMicAndSpeakerUnderTheSessionId) {
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Mic, 1));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic).front(), "call:1");
  EXPECT_TRUE(engine.HasLocalCapture());
  engine.Stop();
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Speaker).empty());
}

TEST_F(EngineDeviceLeaseTest, PlaybackOnlyNeverTakesTheMic) {
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.Start("view:1", CallMediaEngine::SessionSpec::PlaybackOnly(), {}));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  engine.Stop();
}

// Two sessions in one process: the second is refused the mic (named holder) but still plays out —
// running both is allowed, doubling the mic is not (policy lives in the arbiter, L003 / L011).
TEST_F(EngineDeviceLeaseTest, SecondDuplexSessionIsRefusedTheMicButKeepsTheSpeaker) {
  auto first_owned = MakeEngine();
  CallMediaEngine& first = *first_owned;
  auto second_owned = MakeEngine();
  CallMediaEngine& second = *second_owned;
  ASSERT_TRUE(first.StartSfu("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Mic, 1));
  ASSERT_TRUE(second.StartSfu("call:2", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 2));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic), std::vector<std::string>{"call:1"});
  EXPECT_FALSE(second.HasLocalCapture());
  second.Stop();
  first.Stop();
}

// The UI toggles the camera; the open (slow here) must never run on its thread.
TEST_F(EngineDeviceLeaseTest, CameraOpensOffTheCallerThreadAndPublishesAPreview) {
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
  backend_->camera_open_delay = std::chrono::milliseconds(300);
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(100));
  EXPECT_TRUE(engine.IsCameraEnabled()) << "requested = on until it fails or is turned off";
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Camera, 1));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Camera).front(), "call:1");
  EXPECT_EQ(backend_->last_display_rotation.load(), 0) << "rotation read on the caller, passed along";
  CallMediaEngine::VideoTileFrame preview;
  bool have_preview = false;
  for (int i = 0; i < 200 && !have_preview; ++i) {
    have_preview = engine.CopyLocalVideoFrame(preview);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(have_preview);
  EXPECT_EQ(preview.width, 64);

  ASSERT_TRUE(engine.SetCameraEnabled(false));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Camera, 0));
  for (int i = 0; i < 100 && engine.CopyLocalVideoFrame(preview); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_FALSE(engine.CopyLocalVideoFrame(preview)) << "preview cleared with the camera";
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, FailedCameraOpenFlipsBackAndIsReportedOnce) {
  backend_->camera_present = false;
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  std::optional<std::string> failure;
  for (int i = 0; i < 200 && !failure; ++i) {
    failure = engine.TakeCameraFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(failure);
  EXPECT_EQ(*failure, "No camera: none attached");
  EXPECT_FALSE(engine.IsCameraEnabled());
  EXPECT_FALSE(engine.TakeCameraFailure()) << "reported once";
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, StopDuringACameraOpenReleasesEverything) {
  backend_->camera_open_delay = std::chrono::milliseconds(200);
  {
    auto engine_owned = MakeEngine();
    CallMediaEngine& engine = *engine_owned;
    ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
    ASSERT_TRUE(engine.SetCameraEnabled(true));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.Stop();
    EXPECT_FALSE(engine.IsCameraEnabled());
  }
  Settle();
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
}

// l3a regression: the camera button needs "can this host encode" before any camera is on.
TEST_F(EngineDeviceLeaseTest, EncoderAvailabilityIsAHostCapabilityNotSessionState) {
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  EXPECT_EQ(engine.VideoEncoderAvailable(), PlatformVideoEncoderSupported());
  ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
  EXPECT_EQ(engine.VideoEncoderAvailable(), PlatformVideoEncoderSupported());
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, ReopenKeepsLeasesAndReplacesEndpoints) {
  auto engine_owned = MakeEngine();
  CallMediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartSfu("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  const auto opens_before = [this]() {
    int n = 0;
    for (const auto& e : log_.Events()) {
      n += e.rfind("open", 0) == 0 ? 1 : 0;
    }
    return n;
  };
  const int before = opens_before();
  engine.RequestAudioDeviceReopen();
  for (int i = 0; i < 400 && opens_before() < before + 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(opens_before(), before + 2) << "mic and speaker reopened";
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic).size(), 1u);
  engine.Stop();
}

} // namespace
} // namespace pbr
