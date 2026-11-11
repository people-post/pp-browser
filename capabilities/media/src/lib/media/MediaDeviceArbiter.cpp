#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/MediaThreadName.h"

#include "common/Diag.h"
#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <optional>
#include <string>
#include <mutex>
#include <thread>
#include <utility>

namespace pbr {
namespace {

logging::Logger& ArbiterLog() {
  static logging::Logger log = logging::getLogger("MediaDeviceArbiter");
  return log;
}

class NullMediaDeviceBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind /*kind*/, const AudioDeviceFormat& /*format*/,
                                            const std::function<bool()>& /*still_wanted*/,
                                            std::string* error) override {
    if (error) {
      *error = "no audio devices (null backend)";
    }
    return nullptr;
  }
};

} // namespace

const char* MediaDeviceKindName(MediaDeviceKind kind) {
  switch (kind) {
    case MediaDeviceKind::Mic:
      return "mic";
    case MediaDeviceKind::Speaker:
      return "speaker";
    case MediaDeviceKind::Camera:
      return "camera";
  }
  return "device";
}

const char* AudioResetResultName(AudioResetResult result) {
  switch (result) {
    case AudioResetResult::Ok:
      return "ok";
    case AudioResetResult::Busy:
      return "busy";
    case AudioResetResult::Timeout:
      return "timeout";
    case AudioResetResult::Failed:
      return "failed";
    case AudioResetResult::Unsupported:
      return "unsupported";
  }
  return "failed";
}

bool MediaDeviceSharePolicy::Exclusive(MediaDeviceKind kind) const {
  switch (kind) {
    case MediaDeviceKind::Mic:
      return exclusive_mic;
    case MediaDeviceKind::Speaker:
      return exclusive_speaker;
    case MediaDeviceKind::Camera:
      return exclusive_camera;
  }
  return true;
}

std::unique_ptr<IMediaDeviceBackend> CreateNullMediaDeviceBackend() {
  return std::make_unique<NullMediaDeviceBackend>();
}

// ---------------------------------------------------------------------------------------------
// Core: one lane (device thread) per device family + the holder table. Leases keep a weak_ptr so one
// outliving the arbiter closes its endpoint inline instead of touching a dead thread.

namespace {

int64_t SteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

ShareWord KindWord(MediaDeviceKind kind) {
  switch (kind) {
    case MediaDeviceKind::Mic:
      return "mic";
    case MediaDeviceKind::Speaker:
      return "speaker";
    case MediaDeviceKind::Camera:
      return "camera";
  }
  return "device";
}

ShareWord OpWord(MediaDeviceOp op) {
  switch (op) {
    case MediaDeviceOp::Open:
      return "open";
    case MediaDeviceOp::Close:
      return "close";
    case MediaDeviceOp::Reopen:
      return "reopen";
  }
  return "other";
}

/** ResetAudio waits for its task this much longer than the backend may take (queue + the task's own checks). */
constexpr std::chrono::milliseconds kAudioResetMargin{500};

} // namespace

/**
 * One device thread and its queue. Every operation carries a budget; the one running longer than
 * its budget makes the lane stuck — later requests are refused at once instead of queueing behind
 * it — until it returns (the lane recovers by itself).
 */
struct MediaDeviceLane {
  struct Task {
    std::function<void()> run;
    std::string what;
    std::chrono::milliseconds budget{0};
    MediaDeviceKind kind = MediaDeviceKind::Speaker;
    MediaDeviceOp op = MediaDeviceOp::Open;
  };

  /** The running task, when it is past its budget. */
  struct Stuck {
    std::string what;
    int64_t ms = 0;
    MediaDeviceKind kind = MediaDeviceKind::Speaker;
    MediaDeviceOp op = MediaDeviceOp::Open;
  };

  explicit MediaDeviceLane(const char* lane_name) : name(lane_name) {}

  const char* name;
  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<Task> tasks;
  bool stopping = false;
  /** Thread gone (or never started): tasks run inline on the caller. */
  bool stopped = false;
  std::thread thread;
  std::thread::id thread_id;
  std::atomic<bool> finished{false};
  /** The running task (under `mu`): since when, for how long it may run, what it is. */
  int64_t busy_since_ms = 0;
  int64_t busy_budget_ms = 0;
  std::string busy_what;
  MediaDeviceKind busy_kind = MediaDeviceKind::Speaker;
  MediaDeviceOp busy_op = MediaDeviceOp::Open;
  /** The running task's overrun was reported (media.device_stuck), once per task. */
  bool stuck_reported = false;

  void Start() {
    thread = std::thread([this]() {
      SetMediaThreadName("pp.devices");
      Run();
    });
    thread_id = thread.get_id();
  }

  void Run() {
    for (;;) {
      Task task;
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [this]() { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          stopped = true;
          break;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
        busy_since_ms = SteadyMs();
        busy_budget_ms = task.budget.count();
        busy_what = task.what;
        busy_kind = task.kind;
        busy_op = task.op;
        stuck_reported = false;
      }
      task.run();
      int64_t took_ms = 0;
      {
        std::lock_guard lock(mu);
        took_ms = SteadyMs() - busy_since_ms;
        busy_since_ms = 0;
        busy_what.clear();
      }
      if (task.budget.count() > 0 && took_ms > task.budget.count()) {
        ArbiterLog().warning << name << " devices responding again: " << task.what << " took " << took_ms << "ms";
        DiagLine("media.device_back")
            .Add("kind", KindWord(task.kind))
            .Add("op", OpWord(task.op))
            .Add("ms", took_ms)
            .Emit();
      }
    }
    finished.store(true, std::memory_order_release);
  }

  /** The running task when it is past its budget (the lane cannot take work); nullopt when it can. */
  std::optional<Stuck> StuckNow() {
    Stuck stuck;
    bool report = false;
    {
      std::lock_guard lock(mu);
      if (busy_since_ms == 0 || busy_budget_ms <= 0) {
        return std::nullopt;
      }
      const int64_t ms = SteadyMs() - busy_since_ms;
      if (ms <= busy_budget_ms) {
        return std::nullopt;
      }
      stuck = Stuck{busy_what, ms, busy_kind, busy_op};
      report = !std::exchange(stuck_reported, true);
    }
    if (report) {
      ArbiterLog().warning << name << " devices not responding: " << stuck.what << " for " << stuck.ms << "ms";
      DiagLine("media.device_stuck")
          .Add("kind", KindWord(stuck.kind))
          .Add("op", OpWord(stuck.op))
          .Add("ms", stuck.ms)
          .Emit();
    }
    return stuck;
  }

  /** Why the lane cannot take work now; empty when it can. */
  std::string StuckReason() {
    const auto stuck = StuckNow();
    return stuck ? stuck->what + " for " + std::to_string(stuck->ms) + "ms" : std::string();
  }

  /** Queue on the lane; runs inline when the thread is gone. */
  void Post(Task task) {
    {
      std::lock_guard lock(mu);
      if (!stopped) {
        tasks.push_back(std::move(task));
        cv.notify_one();
        return;
      }
    }
    task.run();
  }

  /**
   * Run on the lane and wait at most `budget` (inline on the lane itself or after Shutdown). False
   * when the budget ran out: the task still runs later and must cope with nobody waiting.
   */
  bool RunAndWait(const std::string& what, std::chrono::milliseconds budget, MediaDeviceKind kind, MediaDeviceOp op,
                  const std::function<void()>& run) {
    if (std::this_thread::get_id() == thread_id) {
      run();
      return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished_future = done->get_future();
    Post(Task{[run, done]() {
                run();
                done->set_value();
              },
              what, budget, kind, op});
    return finished_future.wait_for(budget) == std::future_status::ready;
  }

  /** Stop after the queued work; false when it was still running at `deadline` (then detached). */
  bool Stop(std::optional<std::chrono::steady_clock::time_point> deadline) {
    {
      std::lock_guard lock(mu);
      stopping = true;
      cv.notify_all();
    }
    if (!thread.joinable()) {
      return true;
    }
    if (deadline) {
      while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < *deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!finished.load(std::memory_order_acquire)) {
        ArbiterLog().warning << name << " device thread still busy at shutdown — detaching (process exit must follow)";
        thread.detach();
        return false;
      }
    }
    thread.join();
    return true;
  }
};

struct MediaDeviceArbiter::Core {
  struct Holder {
    uint64_t id = 0;
    std::string label;
  };

  std::unique_ptr<IMediaDeviceBackend> backend;
  MediaDeviceSharePolicy policy;
  MediaDeviceBudgets budgets;

  /** Mic + speaker share a lane (a voice-processing unit opens both; ringtone vs call speaker). */
  MediaDeviceLane audio{"audio"};
  MediaDeviceLane camera{"camera"};

  mutable std::mutex mu;
  std::vector<std::pair<MediaDeviceKind, Holder>> holders;
  uint64_t next_id = 1;

  MediaDeviceLane& LaneFor(MediaDeviceKind kind) { return kind == MediaDeviceKind::Camera ? camera : audio; }

  std::chrono::milliseconds OpenBudget(MediaDeviceKind kind) const {
    return kind == MediaDeviceKind::Camera ? budgets.open_camera : budgets.open_audio;
  }

  /** Refused while the kind's lane is stuck: "camera not responding (close camera for call:a for 4100ms)". */
  Roe<void> Responsive(MediaDeviceKind kind) {
    if (const std::string stuck = LaneFor(kind).StuckReason(); !stuck.empty()) {
      return Error(std::string(MediaDeviceKindName(kind)) + " not responding (" + stuck + ")");
    }
    return {};
  }

  Roe<uint64_t> Hold(MediaDeviceKind kind, const std::string& label) {
    std::lock_guard lock(mu);
    if (policy.Exclusive(kind)) {
      for (const auto& [held_kind, holder] : holders) {
        if (held_kind == kind) {
          return Error(std::string(MediaDeviceKindName(kind)) + " held by " + holder.label);
        }
      }
    }
    const uint64_t id = next_id++;
    holders.push_back({kind, Holder{id, label}});
    return id;
  }

  void Unhold(uint64_t id) {
    std::lock_guard lock(mu);
    holders.erase(
        std::remove_if(holders.begin(), holders.end(), [id](const auto& entry) { return entry.second.id == id; }),
        holders.end());
  }

  bool Held(MediaDeviceKind kind) const {
    std::lock_guard lock(mu);
    return std::any_of(holders.begin(), holders.end(), [kind](const auto& entry) { return entry.first == kind; });
  }
};

// ---------------------------------------------------------------------------------------------

namespace {

/** Shared by audio and camera leases: holder slot + endpoint guarded for I/O vs swap / close. */
template <class Endpoint>
struct LeaseSlot {
  std::weak_ptr<MediaDeviceArbiter::Core> core;
  MediaDeviceKind kind = MediaDeviceKind::Speaker;
  std::string holder;
  uint64_t id = 0;

  mutable std::mutex io_mu;
  std::unique_ptr<Endpoint> endpoint;
  std::string open_error;
  /** Released (or abandoned by a waiter that ran out of time): a late open closes what it opened. */
  bool released = false;

  bool Released() const {
    std::lock_guard lock(io_mu);
    return released;
  }

  /** Lane only. An endpoint opened after the lease was released closes here, on the lane. */
  void Install(std::unique_ptr<Endpoint> opened, const std::string& error) {
    std::unique_ptr<Endpoint> late;
    {
      std::lock_guard lock(io_mu);
      if (released) {
        late = std::move(opened);
      } else {
        endpoint = std::move(opened);
        open_error = endpoint ? std::string() : error;
      }
    }
  }

  std::unique_ptr<Endpoint> TakeEndpoint() {
    std::lock_guard lock(io_mu);
    return std::move(endpoint);
  }

  /**
   * Unhold now: an exclusive re-acquire right after release (session rebuild) must not be refused
   * while the close is still queued. FIFO keeps that close ahead of the next open.
   */
  void Release() {
    std::shared_ptr<Endpoint> closing;
    {
      std::lock_guard lock(io_mu);
      released = true;
      closing = std::move(endpoint);
    }
    auto live = core.lock();
    if (!live) {
      return; // closes here, inline
    }
    live->Unhold(id);
    if (closing) {
      live->LaneFor(kind).Post(
          MediaDeviceLane::Task{[closing = std::move(closing)]() mutable { closing.reset(); },
                                std::string("close ") + MediaDeviceKindName(kind) + " for " + holder,
                                live->budgets.close, kind, MediaDeviceOp::Close});
    }
  }
};

} // namespace

struct AudioDeviceLease::State : LeaseSlot<IAudioEndpoint> {
  AudioDeviceFormat format;
  std::function<bool()> still_wanted;
  /** Half of a voice-processing unit: never reopened alone. */
  bool voice_duplex = false;

  /** Device thread only (or inline after Shutdown). */
  void OpenOnDevice(IMediaDeviceBackend& backend) {
    if (Released()) {
      return;
    }
    std::string error;
    // A waiter that gave up (or a release) also ends the platform's open retries.
    const auto wanted = [this]() { return !Released() && (!still_wanted || still_wanted()); };
    auto opened = backend.OpenAudio(kind, format, wanted, &error);
    if (!opened) {
      ArbiterLog().info << MediaDeviceKindName(kind) << " open for " << holder << " gave no device: " << error;
    }
    Install(std::move(opened), error);
  }
};

struct CameraDeviceLease::State : LeaseSlot<ICameraEndpoint> {};

AudioDeviceLease::AudioDeviceLease(std::shared_ptr<State> state) : state_(std::move(state)) {}

AudioDeviceLease::~AudioDeviceLease() {
  state_->Release();
}

MediaDeviceKind AudioDeviceLease::Kind() const {
  return state_->kind;
}

const std::string& AudioDeviceLease::Holder() const {
  return state_->holder;
}

bool AudioDeviceLease::HasDevice() const {
  std::lock_guard lock(state_->io_mu);
  return static_cast<bool>(state_->endpoint);
}

std::string AudioDeviceLease::OpenError() const {
  std::lock_guard lock(state_->io_mu);
  return state_->open_error;
}

int AudioDeviceLease::Read(void* dst, int bytes) {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->Read(dst, bytes) : 0;
}

bool AudioDeviceLease::Write(const void* src, int bytes) {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint && state_->endpoint->Write(src, bytes);
}

int AudioDeviceLease::Queued() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->Queued() : 0;
}

void AudioDeviceLease::Clear() {
  std::lock_guard lock(state_->io_mu);
  if (state_->endpoint) {
    state_->endpoint->Clear();
  }
}

bool AudioDeviceLease::IsVoiceDuplex() const {
  return state_->voice_duplex;
}

bool AudioDeviceLease::WithVoiceProcessing(const std::function<void(IVoiceProcessing&)>& fn) {
  std::lock_guard lock(state_->io_mu);
  IVoiceProcessing* vp = state_->endpoint ? state_->endpoint->VoiceProcessing() : nullptr;
  if (!vp) {
    return false;
  }
  fn(*vp);
  return true;
}

Roe<void> AudioDeviceLease::Reopen() {
  if (state_->voice_duplex) {
    return Error("voice duplex half: release the pair and acquire it again");
  }
  auto core = state_->core.lock();
  if (!core) {
    return Error("media device arbiter gone");
  }
  if (auto responsive = core->Responsive(state_->kind); !responsive) {
    return responsive.error();
  }
  auto state = state_;
  const std::string what = std::string("reopen ") + MediaDeviceKindName(state->kind) + " for " + state->holder;
  const bool done = core->audio.RunAndWait(
      what, core->budgets.close + core->budgets.open_audio, state->kind, MediaDeviceOp::Reopen, [state, core]() {
        if (state->Released()) {
          return;
        }
        // Readers / writers see no device from here until the new endpoint is installed.
        state->TakeEndpoint().reset();
        if (const auto settle = core->backend->ReopenSettle(state->kind); settle.count() > 0) {
          std::this_thread::sleep_for(settle);
        }
        state->OpenOnDevice(*core->backend);
      });
  if (!done) {
    return Error(std::string(MediaDeviceKindName(state->kind)) + " not responding (" + what + ")");
  }
  return {};
}

Roe<void> AudioDeviceLease::CloseEndpoint() {
  if (state_->voice_duplex) {
    return Error("voice duplex half: release the pair and acquire it again");
  }
  auto core = state_->core.lock();
  if (!core) {
    return Error("media device arbiter gone");
  }
  if (auto responsive = core->Responsive(state_->kind); !responsive) {
    return responsive.error();
  }
  auto state = state_;
  const std::string what = std::string("close ") + MediaDeviceKindName(state->kind) + " for " + state->holder;
  // Destroyed here, on the device thread: readers / writers see no device from now until a Reopen installs one.
  const bool done = core->audio.RunAndWait(what, core->budgets.close, state->kind, MediaDeviceOp::Close,
                                           [state]() { state->TakeEndpoint().reset(); });
  if (!done) {
    return Error(std::string(MediaDeviceKindName(state->kind)) + " not responding (" + what + ")");
  }
  return {};
}

CameraDeviceLease::CameraDeviceLease(std::shared_ptr<State> state) : state_(std::move(state)) {}

CameraDeviceLease::~CameraDeviceLease() {
  state_->Release();
}

const std::string& CameraDeviceLease::Holder() const {
  return state_->holder;
}

CameraGeometry CameraDeviceLease::Geometry() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->Geometry() : CameraGeometry{};
}

std::string CameraDeviceLease::CaptureError() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->CaptureError() : "camera unavailable";
}

bool CameraDeviceLease::PermissionPending() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint && state_->endpoint->PermissionPending();
}

bool CameraDeviceLease::PermissionDenied() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint && state_->endpoint->PermissionDenied();
}

std::optional<VideoFrameRgba> CameraDeviceLease::NextFrame() {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->NextFrame() : std::nullopt;
}

bool CameraDeviceLease::SetZoom(float factor) {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint && state_->endpoint->SetZoom(factor);
}

CameraZoomState CameraDeviceLease::ZoomState() const {
  std::lock_guard lock(state_->io_mu);
  return state_->endpoint ? state_->endpoint->ZoomState() : CameraZoomState{};
}

// ---------------------------------------------------------------------------------------------

MediaDeviceArbiter::MediaDeviceArbiter(std::unique_ptr<IMediaDeviceBackend> backend, MediaDeviceSharePolicy policy,
                                       MediaDeviceBudgets budgets)
    : core_(std::make_shared<Core>()) {
  core_->backend = backend ? std::move(backend) : CreateNullMediaDeviceBackend();
  core_->policy = policy;
  core_->budgets = budgets;
  core_->audio.Start();
  core_->camera.Start();
}

MediaDeviceArbiter::~MediaDeviceArbiter() {
  (void)Shutdown(std::chrono::milliseconds::max());
}

namespace {
std::once_flag g_default_once;
std::atomic<MediaDeviceArbiter*> g_default{nullptr};
std::mutex g_default_factory_mu;
std::function<std::unique_ptr<IMediaDeviceBackend>()> g_default_factory;
} // namespace

bool MediaDeviceArbiter::SetDefaultBackendFactory(std::function<std::unique_ptr<IMediaDeviceBackend>()> make) {
  std::lock_guard lock(g_default_factory_mu);
  if (g_default.load(std::memory_order_acquire) != nullptr) {
    return false;
  }
  g_default_factory = std::move(make);
  return true;
}

MediaDeviceArbiter& MediaDeviceArbiter::Default() {
  // Never destroyed: leases may be released during static destruction; ShutdownDefault stops the thread.
  std::call_once(g_default_once, []() {
    std::lock_guard lock(g_default_factory_mu);
    g_default.store(new MediaDeviceArbiter(g_default_factory ? g_default_factory() : CreateSdlMediaDeviceBackend()),
                    std::memory_order_release);
  });
  return *g_default.load(std::memory_order_acquire);
}

bool MediaDeviceArbiter::ShutdownDefault(std::chrono::milliseconds budget) {
  MediaDeviceArbiter* instance = g_default.load(std::memory_order_acquire);
  return instance ? instance->Shutdown(budget) : true;
}

namespace {

/** "mic open for call:a" — what a lane is doing, for a stuck report. */
std::string OpenWhat(MediaDeviceKind kind, const std::string& holder) {
  return std::string("open ") + MediaDeviceKindName(kind) + " for " + holder;
}

Error NotResponding(MediaDeviceKind kind, const std::string& what, std::chrono::milliseconds budget) {
  return Error(std::string(MediaDeviceKindName(kind)) + " not responding (" + what + " took over " +
               std::to_string(budget.count()) + "ms)");
}

} // namespace

Roe<std::unique_ptr<AudioDeviceLease>> MediaDeviceArbiter::AcquireAudio(const AudioLeaseRequest& request) {
  if (auto responsive = core_->Responsive(request.kind); !responsive) {
    ArbiterLog().warning << "refused " << MediaDeviceKindName(request.kind) << " to " << request.holder << ": "
                         << responsive.error().message;
    return responsive.error();
  }
  auto held = core_->Hold(request.kind, request.holder);
  if (!held) {
    ArbiterLog().info << "refused " << MediaDeviceKindName(request.kind) << " to " << request.holder << ": "
                      << held.error().message;
    return held.error();
  }
  auto state = std::make_shared<AudioDeviceLease::State>();
  state->core = core_;
  state->kind = request.kind;
  state->holder = request.holder;
  state->format = request.format;
  state->still_wanted = request.still_wanted;
  state->id = *held;
  auto core = core_;
  auto lease = std::unique_ptr<AudioDeviceLease>(new AudioDeviceLease(state));
  const std::string what = OpenWhat(request.kind, request.holder);
  const auto budget = core_->OpenBudget(request.kind);
  if (!core_->audio.RunAndWait(what, budget, request.kind, MediaDeviceOp::Open,
                               [state, core]() { state->OpenOnDevice(*core->backend); })) {
    ArbiterLog().warning << what << " gave up after " << budget.count() << "ms";
    return NotResponding(request.kind, what, budget); // `lease` releases: a late open closes on the lane
  }
  return lease;
}

Roe<MediaDeviceArbiter::VoiceDuplexLeases> MediaDeviceArbiter::AcquireVoiceDuplex(const AudioLeaseRequest& request) {
  if (auto responsive = core_->Responsive(MediaDeviceKind::Mic); !responsive) {
    ArbiterLog().warning << "refused voice duplex to " << request.holder << ": " << responsive.error().message;
    return responsive.error();
  }
  auto mic_held = core_->Hold(MediaDeviceKind::Mic, request.holder);
  if (!mic_held) {
    ArbiterLog().info << "refused voice duplex to " << request.holder << ": " << mic_held.error().message;
    return mic_held.error();
  }
  auto speaker_held = core_->Hold(MediaDeviceKind::Speaker, request.holder);
  if (!speaker_held) {
    core_->Unhold(*mic_held);
    ArbiterLog().info << "refused voice duplex to " << request.holder << ": " << speaker_held.error().message;
    return speaker_held.error();
  }
  const auto make_state = [&](MediaDeviceKind kind, uint64_t id) {
    auto state = std::make_shared<AudioDeviceLease::State>();
    state->core = core_;
    state->kind = kind;
    state->holder = request.holder;
    state->format = request.format;
    state->still_wanted = request.still_wanted;
    state->id = id;
    state->voice_duplex = true;
    return state;
  };
  auto mic = make_state(MediaDeviceKind::Mic, *mic_held);
  auto speaker = make_state(MediaDeviceKind::Speaker, *speaker_held);
  // The waiter may give up: what the lane reports back lives as long as the task.
  struct Outcome {
    std::mutex mu;
    std::string error;
    bool opened = false;
  };
  auto outcome = std::make_shared<Outcome>();
  auto core = core_;
  const AudioDeviceFormat format = request.format;
  const std::string what = "open voice duplex for " + request.holder;
  const bool done = core_->audio.RunAndWait(what, core_->budgets.open_audio, MediaDeviceKind::Mic, MediaDeviceOp::Open,
                                            [mic, speaker, core, format, outcome]() {
                                              if (mic->Released()) {
                                                return;
                                              }
                                              std::string error;
                                              VoiceDuplexEndpoints pair =
                                                  core->backend->OpenVoiceDuplex(format, &error);
                                              const bool opened = pair.mic && pair.speaker;
                                              if (opened) {
                                                mic->Install(std::move(pair.mic), {});
                                                speaker->Install(std::move(pair.speaker), {});
                                              }
                                              std::lock_guard lock(outcome->mu);
                                              outcome->opened = opened;
                                              outcome->error = error;
                                              // A half-open pair closes here, on the lane.
                                            });
  VoiceDuplexLeases leases;
  leases.mic = std::unique_ptr<AudioDeviceLease>(new AudioDeviceLease(std::move(mic)));
  leases.speaker = std::unique_ptr<AudioDeviceLease>(new AudioDeviceLease(std::move(speaker)));
  if (!done) {
    ArbiterLog().warning << what << " gave up after " << core_->budgets.open_audio.count() << "ms";
    return NotResponding(MediaDeviceKind::Mic, what, core_->budgets.open_audio); // the leases release
  }
  std::lock_guard lock(outcome->mu);
  if (!outcome->opened) {
    return Error(outcome->error.empty() ? std::string("voice duplex unavailable") : outcome->error);
  }
  return leases;
}

Roe<std::unique_ptr<CameraDeviceLease>> MediaDeviceArbiter::AcquireCamera(const CameraLeaseRequest& request) {
  if (auto responsive = core_->Responsive(MediaDeviceKind::Camera); !responsive) {
    ArbiterLog().warning << "refused camera to " << request.holder << ": " << responsive.error().message;
    return responsive.error();
  }
  auto held = core_->Hold(MediaDeviceKind::Camera, request.holder);
  if (!held) {
    ArbiterLog().info << "refused camera to " << request.holder << ": " << held.error().message;
    return held.error();
  }
  auto state = std::make_shared<CameraDeviceLease::State>();
  state->core = core_;
  state->kind = MediaDeviceKind::Camera;
  state->holder = request.holder;
  state->id = *held;
  auto core = core_;
  const CameraRequestFormat format = request.format;
  auto lease = std::unique_ptr<CameraDeviceLease>(new CameraDeviceLease(state));
  const std::string what = OpenWhat(MediaDeviceKind::Camera, request.holder);
  const bool done = core_->camera.RunAndWait(what, core_->budgets.open_camera, MediaDeviceKind::Camera,
                                             MediaDeviceOp::Open, [state, core, format]() {
                                               if (state->Released()) {
                                                 return;
                                               }
                                               std::string error;
                                               auto opened = core->backend->OpenCamera(format, &error);
                                               state->Install(std::move(opened), error);
                                             });
  if (!done) {
    ArbiterLog().warning << what << " gave up after " << core_->budgets.open_camera.count() << "ms";
    return NotResponding(MediaDeviceKind::Camera, what, core_->budgets.open_camera);
  }
  std::string error;
  {
    std::lock_guard lock(state->io_mu);
    if (state->endpoint) {
      return lease;
    }
    error = state->open_error.empty() ? std::string("no camera") : state->open_error;
  }
  lease.reset(); // unholds
  return Error(error);
}

std::vector<std::string> MediaDeviceArbiter::Holders(MediaDeviceKind kind) const {
  std::lock_guard lock(core_->mu);
  std::vector<std::string> out;
  for (const auto& [held_kind, holder] : core_->holders) {
    if (held_kind == kind) {
      out.push_back(holder.label);
    }
  }
  return out;
}

bool MediaDeviceArbiter::DrainAudio(std::chrono::milliseconds budget) {
  // FIFO: when this no-op runs, every close queued before it has run.
  return core_->audio.RunAndWait("drain audio", budget, MediaDeviceKind::Mic, MediaDeviceOp::Close, []() {});
}

bool MediaDeviceArbiter::CanResetAudio() const {
  return core_->backend->SupportsAudioReset();
}

AudioResetResult MediaDeviceArbiter::ResetAudio(std::chrono::milliseconds budget, std::string* error) {
  std::string scratch;
  std::string& why = error ? *error : scratch;
  if (!core_->backend->SupportsAudioReset()) {
    why = "unsupported";
    return AudioResetResult::Unsupported;
  }
  if (auto responsive = core_->Responsive(MediaDeviceKind::Mic); !responsive) {
    why = responsive.error().message;
    return AudioResetResult::Busy;
  }
  struct Outcome {
    std::mutex mu;
    AudioResetResult result = AudioResetResult::Timeout;
    std::string error;
  };
  auto outcome = std::make_shared<Outcome>();
  auto core = core_;
  // The task holds the device thread while it runs: opens queued meanwhile wait for it (FIFO). Run late (the
  // waiter gave up), it re-checks the holders, so a lease taken meanwhile is never pulled from under its holder.
  const bool done =
      core_->audio.RunAndWait("reset audio", budget + kAudioResetMargin, MediaDeviceKind::Mic, MediaDeviceOp::Reopen,
                              [core, budget, outcome]() {
                                std::string task_error;
                                AudioResetResult result = AudioResetResult::Busy;
                                if (core->Held(MediaDeviceKind::Mic) || core->Held(MediaDeviceKind::Speaker)) {
                                  task_error = "an audio device is held";
                                } else {
                                  result = core->backend->ResetAudio(budget, &task_error);
                                }
                                std::lock_guard lock(outcome->mu);
                                outcome->result = result;
                                outcome->error = task_error;
                              });
  std::lock_guard lock(outcome->mu);
  if (!done) {
    why = "audio devices busy past the budget";
    return AudioResetResult::Timeout;
  }
  why = outcome->error;
  return outcome->result;
}

bool MediaDeviceArbiter::CanAcquire(MediaDeviceKind kind) const {
  if (!core_->LaneFor(kind).StuckReason().empty()) {
    return false;
  }
  return !core_->policy.Exclusive(kind) || !core_->Held(kind);
}

MediaDeviceHealth MediaDeviceArbiter::Health(MediaDeviceKind kind) const {
  MediaDeviceHealth health;
  health.holders = Holders(kind);
  if (const auto stuck = core_->LaneFor(kind).StuckNow()) {
    health.stuck = stuck->what + " for " + std::to_string(stuck->ms) + "ms";
    health.stuck_ms = stuck->ms;
  }
  return health;
}

bool MediaDeviceArbiter::Shutdown(std::chrono::milliseconds budget) {
  std::optional<std::chrono::steady_clock::time_point> deadline;
  if (budget != std::chrono::milliseconds::max()) {
    deadline = std::chrono::steady_clock::now() + budget;
  }
  const bool audio_done = core_->audio.Stop(deadline);
  const bool camera_done = core_->camera.Stop(deadline);
  return audio_done && camera_done;
}

} // namespace pbr
