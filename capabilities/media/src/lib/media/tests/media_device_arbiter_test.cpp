#include "lib/media/MediaEngine.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VideoCodecUnavailable.h"
#include "mesh/types/MediaChannel.h"
#include "common/MeshAlias.h"
#include "common/Logger.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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

/** A fake voice-processing unit shared by its two halves; logs its close once both are gone. */
class FakeVoiceUnit final : public IVoiceProcessing {
public:
  explicit FakeVoiceUnit(DeviceLog& log) : log_(log) {}
  ~FakeVoiceUnit() override {
    log_.Enter("close duplex");
    log_.Leave();
  }
  size_t RenderChunkBytes() const override { return 4096 * sizeof(int16_t); }
  uint64_t PlayoutUnderruns() const override { return 7; }
  std::string TakeDiag() override { return {}; }
  bool TakeDeviceChanged() override { return device_changed.exchange(false); }
  std::atomic<bool> device_changed{false};

private:
  DeviceLog& log_;
};

class FakeVoiceHalf final : public IAudioEndpoint {
public:
  explicit FakeVoiceHalf(std::shared_ptr<FakeVoiceUnit> unit) : unit_(std::move(unit)) {}
  /** Paced like a device: one 20 ms chunk per period. */
  int Read(void* dst, int bytes) override {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_read_ < std::chrono::milliseconds(20)) {
      return 0;
    }
    last_read_ = now;
    std::memset(dst, 0, static_cast<size_t>(bytes));
    return bytes;
  }
  bool Write(const void*, int) override { return true; }
  int Queued() const override { return 0; }
  void Clear() override {}
  IVoiceProcessing* VoiceProcessing() override { return unit_.get(); }

private:
  std::shared_ptr<FakeVoiceUnit> unit_;
  std::chrono::steady_clock::time_point last_read_{};
};

/** Holds a device operation until released (bounded, so a failing test cannot hang the run). */
struct HoldGate {
  std::atomic<bool> armed{false};
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
  void Pass() {
    if (!armed.load()) {
      return;
    }
    entered.store(true);
    for (int i = 0; i < 2000 && !release.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
};

/** 64×36 RGBA frames, one per 30 ms. */
class FakeCamera final : public ICameraEndpoint {
public:
  FakeCamera(DeviceLog& log, bool front_facing, HoldGate& close_gate)
      : log_(log), front_facing_(front_facing), close_gate_(close_gate) {}
  ~FakeCamera() override {
    close_gate_.Pass();
    log_.Enter("close camera");
    log_.Leave();
  }
  CameraGeometry Geometry() const override { return CameraGeometry{0, 64, 36, front_facing_}; }
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
  bool front_facing_ = false;
  HoldGate& close_gate_;
  std::chrono::steady_clock::time_point last_{};
};

class FakeBackend final : public IMediaDeviceBackend {
public:
  explicit FakeBackend(DeviceLog& log) : log_(log) {}
  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& format, std::string* error) override {
    last_display_rotation = format.display_rotation_deg;
    last_prefer_front = format.prefer_front_facing ? 1 : 0;
    std::this_thread::sleep_for(camera_open_delay.load());
    log_.Enter("open camera");
    log_.Leave();
    if (!camera_present) {
      *error = "No camera: none attached";
      return nullptr;
    }
    return std::make_unique<FakeCamera>(log_, format.prefer_front_facing, camera_close_gate);
  }
  /** Armed: closing a camera hangs until released (a wedged OS camera close). */
  HoldGate camera_close_gate;
  std::atomic<bool> camera_present{true};
  std::atomic<std::chrono::milliseconds> camera_open_delay{std::chrono::milliseconds(0)};
  std::atomic<int> last_display_rotation{-1};
  /** The facing the last camera open asked for: 1 front, 0 rear, -1 none yet. */
  std::atomic<int> last_prefer_front{-1};
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

  VoiceDuplexEndpoints OpenVoiceDuplex(const AudioDeviceFormat& /*format*/, std::string* error) override {
    if (!duplex) {
      *error = "unsupported";
      return {};
    }
    log_.Enter("open duplex");
    log_.Leave();
    auto unit = std::make_shared<FakeVoiceUnit>(log_);
    {
      std::lock_guard lock(unit_mu_);
      last_unit_ = unit;
    }
    VoiceDuplexEndpoints pair;
    pair.mic = std::make_unique<FakeVoiceHalf>(unit);
    pair.speaker = std::make_unique<FakeVoiceHalf>(unit);
    return pair;
  }
  std::atomic<bool> duplex{false};
  std::shared_ptr<FakeVoiceUnit> LastUnit() {
    std::lock_guard lock(unit_mu_);
    return last_unit_.lock();
  }

private:
  DeviceLog& log_;
  std::mutex unit_mu_;
  std::weak_ptr<FakeVoiceUnit> last_unit_;
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

// --- voice duplex: mic + speaker from one voice-processing unit --------------------------------

TEST_F(MediaDeviceArbiterTest, VoiceDuplexHoldsBothKindsAndClosesOnceBothHalvesGo) {
  backend_->duplex = true;
  auto pair = arbiter_->AcquireVoiceDuplex(Request(MediaDeviceKind::Mic, "call:1"));
  ASSERT_TRUE(pair) << pair.error().message;
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic), std::vector<std::string>{"call:1"});
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Speaker), std::vector<std::string>{"call:1"});
  EXPECT_TRUE(pair->mic->IsVoiceDuplex());
  EXPECT_TRUE(pair->speaker->WithVoiceProcessing([](IVoiceProcessing&) {}));
  EXPECT_FALSE(pair->mic->Reopen()) << "one half cannot reopen a shared unit";
  EXPECT_FALSE(arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "call:2"))) << "the mic stays exclusive";
  pair->mic.reset();
  pair->speaker.reset();
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Speaker).empty());
  Settle();
  EXPECT_EQ(log_.Events(), (std::vector<std::string>{"open duplex", "close duplex"}));
}

TEST_F(MediaDeviceArbiterTest, UnsupportedVoiceDuplexHoldsNothing) {
  auto pair = arbiter_->AcquireVoiceDuplex(Request(MediaDeviceKind::Mic, "call:1"));
  ASSERT_FALSE(pair);
  EXPECT_EQ(pair.error().message, "unsupported");
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Speaker).empty());
}

TEST_F(MediaDeviceArbiterTest, VoiceDuplexIsRefusedWhileTheMicIsHeld) {
  backend_->duplex = true;
  auto mic = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "call:1"));
  ASSERT_TRUE(mic);
  auto pair = arbiter_->AcquireVoiceDuplex(Request(MediaDeviceKind::Mic, "call:2"));
  ASSERT_FALSE(pair);
  EXPECT_EQ(pair.error().message, "mic held by call:1");
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Speaker).empty()) << "no speaker hold left behind";
}

// --- budgets: a stuck device family is refused at once and recovers by itself (L016) ------------

class StuckDeviceTest : public ::testing::Test {
protected:
  StuckDeviceTest() {
    auto backend = std::make_unique<FakeBackend>(log_);
    backend_ = backend.get();
    MediaDeviceBudgets budgets;
    budgets.open_audio = std::chrono::milliseconds(300);
    budgets.open_camera = std::chrono::milliseconds(300);
    budgets.close = std::chrono::milliseconds(150);
    arbiter_ = std::make_unique<MediaDeviceArbiter>(std::move(backend), MediaDeviceSharePolicy{}, budgets);
  }
  ~StuckDeviceTest() override {
    backend_->camera_close_gate.release.store(true);
    arbiter_.reset();
  }
  static bool WaitUntil(const std::function<bool()>& done) {
    for (int i = 0; i < 400 && !done(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  }
  /** A camera released while its close hangs: the camera family is stuck past the close budget. */
  void WedgeCameraClose() {
    backend_->camera_close_gate.armed.store(true);
    auto camera = arbiter_->AcquireCamera(CameraLeaseRequest{"call:a", {}});
    ASSERT_TRUE(camera) << camera.error().message;
    camera->reset();
    ASSERT_TRUE(WaitUntil([&] { return backend_->camera_close_gate.entered.load(); }));
    ASSERT_TRUE(WaitUntil([&] { return !arbiter_->Health(MediaDeviceKind::Camera).stuck.empty(); }));
  }

  DeviceLog log_;
  FakeBackend* backend_ = nullptr;
  std::unique_ptr<MediaDeviceArbiter> arbiter_;
};

TEST_F(StuckDeviceTest, AStuckCameraCloseIsRefusedAtOnceAndNamed) {
  WedgeCameraClose();
  EXPECT_FALSE(arbiter_->CanAcquire(MediaDeviceKind::Camera));
  const auto from = std::chrono::steady_clock::now();
  auto refused = arbiter_->AcquireCamera(CameraLeaseRequest{"qr-scanner", {}});
  EXPECT_LT(std::chrono::steady_clock::now() - from, std::chrono::milliseconds(100)) << "refused at once";
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().message.find("camera not responding (close camera for call:a"), std::string::npos)
      << refused.error().message;
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty()) << "a refusal holds nothing";
}

TEST_F(StuckDeviceTest, AStuckCameraNeverHoldsTheAudioDevices) {
  WedgeCameraClose();
  EXPECT_TRUE(arbiter_->CanAcquire(MediaDeviceKind::Mic));
  auto mic = arbiter_->AcquireAudio(Request(MediaDeviceKind::Mic, "call:b"));
  ASSERT_TRUE(mic) << mic.error().message;
  EXPECT_TRUE((*mic)->HasDevice());
}

TEST_F(StuckDeviceTest, TheCameraComesBackWhenTheStuckCloseReturns) {
  WedgeCameraClose();
  backend_->camera_close_gate.armed.store(false);
  backend_->camera_close_gate.release.store(true);
  ASSERT_TRUE(WaitUntil([&] { return arbiter_->CanAcquire(MediaDeviceKind::Camera); }));
  EXPECT_TRUE(arbiter_->Health(MediaDeviceKind::Camera).stuck.empty());
  auto camera = arbiter_->AcquireCamera(CameraLeaseRequest{"call:b", {}});
  ASSERT_TRUE(camera) << camera.error().message;
}

TEST_F(StuckDeviceTest, AnOpenPastItsBudgetIsRefusedAndTheLateDeviceCloses) {
  backend_->camera_open_delay = std::chrono::milliseconds(600);
  const auto from = std::chrono::steady_clock::now();
  auto late = arbiter_->AcquireCamera(CameraLeaseRequest{"call:a", {}});
  EXPECT_LT(std::chrono::steady_clock::now() - from, std::chrono::milliseconds(550)) << "gave up at the budget";
  ASSERT_FALSE(late);
  EXPECT_NE(late.error().message.find("camera not responding"), std::string::npos) << late.error().message;
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
  backend_->camera_open_delay = std::chrono::milliseconds(0);
  ASSERT_TRUE(arbiter_->Shutdown(std::chrono::seconds(2)));
  EXPECT_EQ(log_.Events(), (std::vector<std::string>{"open camera", "close camera"})) << "opened late, closed at once";
}

// --- engine takes the leases its session spec asks for -----------------------------------------

class EngineDeviceLeaseTest : public MediaDeviceArbiterTest {
protected:
  /** Engine on the fake devices, with a stub codec: these tests are about devices, not the host's HW. */
  std::unique_ptr<MediaEngine> MakeEngine() {
    auto engine = std::make_unique<MediaEngine>(*arbiter_);
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
  /**
   * The arbiter lists a holder as soon as the hold is granted — before the device opens and the
   * engine installs the lease — so tests about the engine wait on what the engine reports.
   */
  static bool WaitUntil(const std::function<bool()>& done) {
    for (int i = 0; i < 400 && !done(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  }
  static MediaEngine::PacketSendFn NoopSend() {
    return [](const MediaEngine::MediaPacket&) {};
  }
};

TEST_F(EngineDeviceLeaseTest, DuplexHoldsMicAndSpeakerUnderTheSessionId) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Mic, 1));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic).front(), "call:1");
  EXPECT_TRUE(WaitUntil([&] { return engine.HasLocalCapture(); }));
  engine.Stop();
  ASSERT_TRUE(engine.WaitForRetiredSessions(std::chrono::seconds(2))) << "the stopped session let its devices go";
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Speaker).empty());
}

// Echo cancellation: a duplex call takes mic + speaker from the voice-processing unit when the
// backend has one (macOS VPIO), and reports it in media health.
TEST_F(EngineDeviceLeaseTest, DuplexCallUsesTheVoiceProcessingUnit) {
  backend_->duplex = true;
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(WaitUntil([&] { return engine.HasLocalCapture(); }));
  EXPECT_EQ(engine.HealthSnapshot().audio_io, "vpio");
  EXPECT_EQ(engine.HealthSnapshot().io_underruns, 7u);
  const auto events = log_.Events();
  EXPECT_EQ(std::count(events.begin(), events.end(), "open duplex"), 1);
  EXPECT_EQ(std::count(events.begin(), events.end(), "open mic"), 0);
  engine.Stop();
  ASSERT_TRUE(engine.WaitForRetiredSessions(std::chrono::seconds(2)));
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
}

// A default-device change re-acquires the whole pair (a half never reopens alone).
TEST_F(EngineDeviceLeaseTest, DefaultDeviceChangeReacquiresTheVoicePair) {
  backend_->duplex = true;
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(WaitUntil([&] { return engine.HealthSnapshot().audio_io == "vpio"; }));
  auto unit = backend_->LastUnit();
  ASSERT_NE(unit, nullptr);
  unit->device_changed = true;
  unit.reset();
  const auto opens = [&]() {
    const auto events = log_.Events();
    return std::count(events.begin(), events.end(), "open duplex");
  };
  for (int i = 0; i < 400 && opens() < 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(opens(), 2);
  EXPECT_TRUE(WaitUntil([&] { return engine.HealthSnapshot().audio_io == "vpio"; }));
  engine.Stop();
  ASSERT_TRUE(engine.WaitForRetiredSessions(std::chrono::seconds(2)));
  Settle();
  const auto events = log_.Events();
  EXPECT_EQ(std::count(events.begin(), events.end(), "close duplex"), 2);
}

// Without a voice-processing unit (other platforms), a call falls back to separate SDL leases.
TEST_F(EngineDeviceLeaseTest, DuplexCallFallsBackToSeparateLeasesWithoutVoiceProcessing) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  EXPECT_TRUE(WaitUntil([&] { return engine.HealthSnapshot().audio_io == "sdl"; }));
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, PlaybackOnlyNeverTakesTheMic) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.Start("view:1", MediaEngine::SessionSpec::PlaybackOnly(), {}));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty());
  engine.Stop();
}

// Two sessions in one process: the second is refused the mic (named holder) but still plays out —
// running both is allowed, doubling the mic is not (policy lives in the arbiter, L003 / L011).
TEST_F(EngineDeviceLeaseTest, SecondDuplexSessionIsRefusedTheMicButKeepsTheSpeaker) {
  auto first_owned = MakeEngine();
  MediaEngine& first = *first_owned;
  auto second_owned = MakeEngine();
  MediaEngine& second = *second_owned;
  ASSERT_TRUE(first.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Mic, 1));
  ASSERT_TRUE(second.StartDuplex("call:2", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 2));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic), std::vector<std::string>{"call:1"});
  EXPECT_FALSE(second.HasLocalCapture());
  second.Stop();
  first.Stop();
}

// The UI toggles the camera; the open (slow here) must never run on its thread.
TEST_F(EngineDeviceLeaseTest, CameraOpensOffTheCallerThreadAndPublishesAPreview) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  backend_->camera_open_delay = std::chrono::milliseconds(300);
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(100));
  EXPECT_TRUE(engine.IsCameraEnabled()) << "requested = on until it fails or is turned off";
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Camera, 1));
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Camera).front(), "call:1");
  // The holder is listed when the request is queued; the device thread records the rotation when
  // it actually runs the (delayed) open — wait for that instead of racing it (flaked on CI).
  for (int i = 0; i < 400 && backend_->last_display_rotation.load() == -1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(backend_->last_display_rotation.load(), 0) << "rotation read on the caller, passed along";
  MediaEngine::VideoTileFrame preview;
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
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  std::optional<CameraFailure> failure;
  for (int i = 0; i < 200 && !failure; ++i) {
    failure = engine.TakeCameraFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, CameraFailure::Kind::Failed);
  EXPECT_EQ(failure->detail, "No camera: none attached");
  EXPECT_FALSE(engine.IsCameraEnabled());
  EXPECT_FALSE(engine.TakeCameraFailure()) << "reported once";
  engine.Stop();
}

// Phones: the flip button re-takes the camera with the other facing; the next call starts front.
TEST_F(EngineDeviceLeaseTest, FlipCameraReopensWithTheOtherFacing) {
  const auto camera_opens = [this]() {
    const auto events = log_.Events();
    return std::count(events.begin(), events.end(), "open camera");
  };
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 1; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 1) << "calls open the front camera first";
  EXPECT_TRUE(WaitUntil([&] { return engine.CameraFrontFacing(); }));

  ASSERT_TRUE(engine.FlipCamera());
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 2; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 0);
  EXPECT_TRUE(WaitUntil([&] { return !engine.CameraFrontFacing(); }));
  EXPECT_TRUE(engine.IsCameraEnabled());
  const auto events = log_.Events();
  EXPECT_EQ(std::count(events.begin(), events.end(), "close camera"), 1) << "the front camera was released";
  EXPECT_TRUE(WaitHolders(MediaDeviceKind::Camera, 1));

  ASSERT_TRUE(engine.FlipCamera());
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 3; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 1) << "flips back";
  EXPECT_TRUE(WaitUntil([&] { return engine.CameraFrontFacing(); }));
  ASSERT_TRUE(engine.FlipCamera());
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 4; }));
  engine.Stop();

  ASSERT_TRUE(engine.StartDuplex("call:2", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 5; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 1) << "a new call starts with the front camera";
  engine.Stop();
}

// A flip whose rear camera does not open leaves the camera off, and the next open asks for the front again.
TEST_F(EngineDeviceLeaseTest, AFailedReopenAfterAFlipGoesBackToTheFrontCamera) {
  const auto camera_opens = [this]() {
    const auto events = log_.Events();
    return std::count(events.begin(), events.end(), "open camera");
  };
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 1; }));
  EXPECT_TRUE(WaitUntil([&] { return engine.CameraFrontFacing(); }));

  backend_->camera_present = false;
  ASSERT_TRUE(engine.FlipCamera());
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 2; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 0) << "the rear camera was asked for";
  std::optional<CameraFailure> failure;
  for (int i = 0; i < 200 && !failure; ++i) {
    failure = engine.TakeCameraFailure();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, CameraFailure::Kind::Failed);
  EXPECT_FALSE(engine.IsCameraEnabled());

  backend_->camera_present = true;
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 3; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 1) << "front again, not the facing that failed";
  engine.Stop();
}

// A tap while the first open is still pending is refused: the facing is not known yet, nothing changes.
TEST_F(EngineDeviceLeaseTest, FlipCameraWhileStillOpeningIsRefused) {
  const auto camera_opens = [this]() {
    const auto events = log_.Events();
    return std::count(events.begin(), events.end(), "open camera");
  };
  backend_->camera_open_delay = std::chrono::milliseconds(300);
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(engine.FlipCamera()) << "still opening";
  ASSERT_TRUE(WaitUntil([&] { return camera_opens() == 1 && engine.CameraFrontFacing(); }));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(camera_opens(), 1) << "the refused tap did not re-open";
  EXPECT_EQ(backend_->last_prefer_front.load(), 1);
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, FlipCameraWhileTheCameraIsOffAsksForNothing) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  EXPECT_FALSE(engine.FlipCamera()) << "no call media";
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  EXPECT_FALSE(engine.FlipCamera()) << "camera off";
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(backend_->last_prefer_front.load(), -1);
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Camera, 1));
  EXPECT_TRUE(WaitUntil([&] { return backend_->last_prefer_front.load() != -1; }));
  EXPECT_EQ(backend_->last_prefer_front.load(), 1) << "a refused flip leaves the facing alone";
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, StopDuringACameraOpenReleasesEverything) {
  backend_->camera_open_delay = std::chrono::milliseconds(200);
  {
    auto engine_owned = MakeEngine();
    MediaEngine& engine = *engine_owned;
    ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
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
  MediaEngine& engine = *engine_owned;
  EXPECT_EQ(engine.VideoEncoderAvailable(), PlatformVideoEncoderSupported());
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  EXPECT_EQ(engine.VideoEncoderAvailable(), PlatformVideoEncoderSupported());
  engine.Stop();
}

TEST_F(EngineDeviceLeaseTest, ReopenKeepsLeasesAndReplacesEndpoints) {
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(WaitHolders(MediaDeviceKind::Speaker, 1));
  const auto opens_before = [this]() {
    int n = 0;
    for (const auto& e : log_.Events()) {
      n += e.rfind("open", 0) == 0 ? 1 : 0;
    }
    return n;
  };
  // The arbiter lists a holder when the hold is granted, before the device opens: count only once
  // the first mic + speaker opens are in, or a late first open is miscounted as a reopen (CI flake).
  ASSERT_TRUE(WaitUntil([&] { return opens_before() >= 2; }));
  const int before = opens_before();
  engine.RequestAudioDeviceReopen();
  for (int i = 0; i < 400 && opens_before() < before + 2; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(opens_before(), before + 2) << "mic and speaker reopened";
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic).size(), 1u);
  engine.Stop();
}

/** Encoder stub whose ConfigureEncoder blocks until released — a camera/encoder step that hangs (B128). */
class HangingEncoder final : public IVideoCodec {
public:
  struct Gate {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
  };
  explicit HangingEncoder(std::shared_ptr<Gate> gate) : gate_(std::move(gate)) {}
  std::string BackendName() const override { return "hanging-encoder"; }
  bool HasEncoder() const override { return false; }
  bool HasDecoder() const override { return false; }
  Roe<void> ConfigureEncoder(int, int, int) override {
    gate_->entered.store(true);
    // Bounded so a failing test cannot hang the run.
    for (int i = 0; i < 2000 && !gate_->release.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Error("no encoder");
  }
  Roe<void> ConfigureDecoder() override { return Error("no decoder"); }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override { return Error("no encoder"); }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return Error("no decoder"); }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  std::shared_ptr<Gate> gate_;
};

/** Collects the engine channel's lines (logger handlers cannot be removed: kept for the process). */
struct EngineLogCapture : logging::Handler {
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mu);
    lines.push_back(message);
  }
  bool Has(const std::string& needle) {
    std::lock_guard lock(mu);
    return std::any_of(lines.begin(), lines.end(),
                       [&](const std::string& line) { return line.find(needle) != std::string::npos; });
  }
  std::mutex mu;
  std::vector<std::string> lines;
};

// #452 / L016: a camera step that hangs (B128) never holds the caller of Stop — the calls owner, which
// must go on with the next invite. Stop returns at once; the stopped session cleans up on its own
// thread, says where its video thread is stuck, and lets the mic go first, so the next call starts
// with a mic while the old camera is still stuck.
TEST_F(EngineDeviceLeaseTest, StopNeverWaitsOnAStuckCameraAndTheNextCallGetsTheMic) {
  auto capture = std::make_shared<EngineLogCapture>();
  logging::getLogger("MediaEngine").addHandler(capture);
  auto gate = std::make_shared<HangingEncoder::Gate>();
  auto engine = std::make_unique<MediaEngine>(*arbiter_);
  engine->SetVideoCodecFactoryForTest([gate] { return std::make_unique<HangingEncoder>(gate); });
  ASSERT_TRUE(engine->StartDuplex("call:stuck", NoopSend()));
  ASSERT_TRUE(engine->SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return gate->entered.load(); })) << "the video thread reached the encoder";

  const auto stop_from = std::chrono::steady_clock::now();
  engine->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - stop_from, std::chrono::milliseconds(200)) << "Stop waited";
  EXPECT_FALSE(engine->IsActive());

  bool warned = false;
  for (int i = 0; i < 700 && !warned; ++i) {  // ~3.5 s
    warned = capture->Has("media stop join_video slow stage=configure_encoder");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(warned);
  EXPECT_FALSE(engine->WaitForRetiredSessions(std::chrono::milliseconds(0))) << "still stuck";
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Mic).empty()) << "the stuck session let the mic go";

  ASSERT_TRUE(engine->StartDuplex("call:next", NoopSend()));
  EXPECT_TRUE(WaitUntil([&] { return engine->HasLocalCapture(); })) << "the next call has a mic";
  EXPECT_EQ(arbiter_->Holders(MediaDeviceKind::Mic), std::vector<std::string>{"call:next"});

  gate->release.store(true);
  engine->Stop();
  EXPECT_TRUE(engine->WaitForRetiredSessions(std::chrono::seconds(3))) << "both sessions finish once the step does";
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
}

// L016: a camera busy with someone else (here the stuck session of the call before) is not a failure:
// the camera stays wanted, the person is told once, and video comes on by itself when it is free.
TEST_F(EngineDeviceLeaseTest, ABusyCameraComesOnByItselfWhenItIsFree) {
  auto scanner = arbiter_->AcquireCamera(CameraLeaseRequest{"qr-scanner", {}});
  ASSERT_TRUE(scanner) << scanner.error().message;
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return engine.CameraWaitingForDevice(); }));
  EXPECT_TRUE(engine.IsCameraEnabled()) << "still wanted";
  const auto failure = engine.TakeCameraFailure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, CameraFailure::Kind::Busy) << failure->detail;
  scanner->reset();
  EXPECT_TRUE(
      WaitUntil([&] { return arbiter_->Holders(MediaDeviceKind::Camera) == std::vector<std::string>{"call:1"}; }));
  MediaEngine::VideoTileFrame preview;
  EXPECT_TRUE(WaitUntil([&] { return engine.CopyLocalVideoFrame(preview); })) << "frames flow";
  EXPECT_FALSE(engine.CameraWaitingForDevice());
  EXPECT_FALSE(engine.TakeCameraFailure()) << "told once";
  engine.Stop();
}

// Turning the camera off while it waits for the device withdraws the wish: it does not come on later.
TEST_F(EngineDeviceLeaseTest, TurningAWaitingCameraOffWithdrawsIt) {
  auto scanner = arbiter_->AcquireCamera(CameraLeaseRequest{"qr-scanner", {}});
  ASSERT_TRUE(scanner);
  auto engine_owned = MakeEngine();
  MediaEngine& engine = *engine_owned;
  ASSERT_TRUE(engine.StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return engine.CameraWaitingForDevice(); }));
  ASSERT_TRUE(engine.SetCameraEnabled(false));
  scanner->reset();
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(engine.IsCameraEnabled());
  EXPECT_TRUE(arbiter_->Holders(MediaDeviceKind::Camera).empty());
  engine.Stop();
}

// --- hardware codecs that stop responding (L016) ------------------------------------------------

/** An encoder; the instances `hang_from` and later hang in Encode until released (bounded). */
class FlakyEncoder final : public IVideoCodec {
public:
  struct Control {
    std::atomic<int> made{0};
    std::atomic<int> hang_from{1};
    std::atomic<int> hang_until{1}; // instances in [hang_from, hang_until) hang
    std::atomic<bool> release{false};
  };
  explicit FlakyEncoder(std::shared_ptr<Control> control)
      : control_(std::move(control)), index_(control_->made.fetch_add(1)) {}
  std::string BackendName() const override { return "flaky"; }
  bool HasEncoder() const override { return configured_; }
  bool HasDecoder() const override { return false; }
  Roe<void> ConfigureEncoder(int, int, int) override {
    configured_ = true;
    return {};
  }
  Roe<void> ConfigureDecoder() override { return Error("no decoder"); }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override {
    if (index_ >= control_->hang_from.load() && index_ < control_->hang_until.load()) {
      for (int i = 0; i < 1000 && !control_->release.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
    return EncodedAccessUnit{{0x01}, false};
  }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return Error("no decoder"); }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  std::shared_ptr<Control> control_;
  int index_ = 0;
  bool configured_ = false;
};

MediaEngine::HardwareLimits ShortHardwareLimits() {
  MediaEngine::HardwareLimits limits;
  limits.codecs.create = std::chrono::milliseconds(300);
  limits.codecs.configure = std::chrono::milliseconds(300);
  limits.codecs.frame = std::chrono::milliseconds(150);
  limits.codecs.reset = std::chrono::milliseconds(300);
  limits.device_give_up_after = std::chrono::milliseconds(400);
  return limits;
}

TEST_F(EngineDeviceLeaseTest, AHungEncoderIsReplacedAndVideoGoesOn) {
  auto control = std::make_shared<FlakyEncoder::Control>();
  control->hang_from = 0;
  control->hang_until = 1; // the first encoder hangs; its replacement works
  auto engine = std::make_unique<MediaEngine>(*arbiter_);
  engine->SetHardwareLimits(ShortHardwareLimits());
  engine->SetVideoCodecFactoryForTest([control] { return std::make_unique<FlakyEncoder>(control); });
  std::atomic<int> video_sent{0};
  ASSERT_TRUE(engine->StartDuplex("call:1", [&](const MediaEngine::MediaPacket& packet) {
    if (mesh::IsVideoChannel(packet.channel_id)) {
      video_sent.fetch_add(1);
    }
  }));
  ASSERT_TRUE(engine->SetCameraEnabled(true));
  EXPECT_TRUE(WaitUntil([&] { return video_sent.load() >= 3; })) << "video flows on the replacement";
  EXPECT_EQ(control->made.load(), 2);
  EXPECT_TRUE(engine->IsCameraEnabled());
  EXPECT_FALSE(engine->TakeCameraFailure());
  control->release = true;
  engine->Stop();
}

TEST_F(EngineDeviceLeaseTest, AnEncoderThatKeepsHangingGivesTheCameraUp) {
  auto control = std::make_shared<FlakyEncoder::Control>();
  control->hang_from = 0;
  control->hang_until = 100;
  auto engine = std::make_unique<MediaEngine>(*arbiter_);
  engine->SetHardwareLimits(ShortHardwareLimits());
  engine->SetVideoCodecFactoryForTest([control] { return std::make_unique<FlakyEncoder>(control); });
  ASSERT_TRUE(engine->StartDuplex("call:1", NoopSend()));
  ASSERT_TRUE(engine->SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return !engine->IsCameraEnabled(); })) << "given up";
  EXPECT_EQ(control->made.load(), 4) << "the first encoder and three replacements";
  const auto failure = engine->TakeCameraFailure();
  ASSERT_TRUE(failure);
  EXPECT_EQ(failure->kind, CameraFailure::Kind::NotResponding) << failure->detail;
  EXPECT_FALSE(engine->CameraWaitingForDevice());
  EXPECT_TRUE(WaitHolders(MediaDeviceKind::Camera, 0)) << "the camera is let go";
  control->release = true;
  engine->Stop();
}

TEST_F(StuckDeviceTest, ACameraStuckTooLongIsGivenUpWithARestartHint) {
  WedgeCameraClose();
  MediaEngine engine(*arbiter_);
  engine.SetHardwareLimits(ShortHardwareLimits());
  engine.SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test stub"); });
  ASSERT_TRUE(engine.StartDuplex("call:b", [](const MediaEngine::MediaPacket&) {}));
  ASSERT_TRUE(engine.SetCameraEnabled(true));
  ASSERT_TRUE(WaitUntil([&] { return !engine.IsCameraEnabled(); })) << "given up";
  std::optional<CameraFailure> failure;
  std::optional<CameraFailure> last;
  while ((failure = engine.TakeCameraFailure())) {
    last = failure;
  }
  ASSERT_TRUE(last);
  EXPECT_EQ(last->kind, CameraFailure::Kind::NotResponding) << last->detail;
  EXPECT_FALSE(engine.CameraWaitingForDevice());
  engine.Stop();
  backend_->camera_close_gate.release.store(true);
  EXPECT_TRUE(engine.WaitForRetiredSessions(std::chrono::seconds(3)));
}

} // namespace
} // namespace pbr
