#include "domain/media/MediaDeviceArbiter.h"

#include "common/Logger.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

namespace pbr {
namespace {

logging::Logger& ArbiterLog() {
  static logging::Logger log = logging::getLogger("MediaDeviceArbiter");
  return log;
}

class NullAudioDeviceBackend final : public IAudioDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> Open(MediaDeviceKind /*kind*/, const AudioDeviceFormat& /*format*/,
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
  }
  return "device";
}

bool MediaDeviceSharePolicy::Exclusive(MediaDeviceKind kind) const {
  return kind == MediaDeviceKind::Mic ? exclusive_mic : exclusive_speaker;
}

std::unique_ptr<IAudioDeviceBackend> CreateNullAudioDeviceBackend() {
  return std::make_unique<NullAudioDeviceBackend>();
}

// ---------------------------------------------------------------------------------------------
// Core: device thread + holder table. Leases keep a weak_ptr so one outliving the arbiter closes
// its endpoint inline instead of touching a dead thread.

struct MediaDeviceArbiter::Core {
  struct Holder {
    uint64_t id = 0;
    std::string label;
  };

  std::unique_ptr<IAudioDeviceBackend> backend;
  MediaDeviceSharePolicy policy;

  mutable std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  std::vector<std::pair<MediaDeviceKind, Holder>> holders;
  uint64_t next_id = 1;
  bool stopping = false;
  /** Device thread gone (or never started): tasks run inline on the caller. */
  bool stopped = false;
  std::thread thread;
  std::thread::id thread_id;
  std::atomic<bool> finished{false};

  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [this]() { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          stopped = true;
          break;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
    finished.store(true, std::memory_order_release);
  }

  /** Queue on the device thread; runs inline when the thread is gone. */
  void Post(std::function<void()> task) {
    {
      std::lock_guard lock(mu);
      if (!stopped) {
        tasks.push_back(std::move(task));
        cv.notify_one();
        return;
      }
    }
    task();
  }

  /** Run on the device thread and wait (inline on the device thread itself or after Shutdown). */
  void RunOnDevice(const std::function<void()>& task) {
    if (std::this_thread::get_id() == thread_id) {
      task();
      return;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto finished_future = done->get_future();
    Post([task, done]() {
      task();
      done->set_value();
    });
    finished_future.wait();
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
    holders.erase(std::remove_if(holders.begin(), holders.end(),
                                 [id](const auto& entry) { return entry.second.id == id; }),
                  holders.end());
  }
};

// ---------------------------------------------------------------------------------------------

struct AudioDeviceLease::State {
  std::weak_ptr<MediaDeviceArbiter::Core> core;
  MediaDeviceKind kind = MediaDeviceKind::Speaker;
  std::string holder;
  AudioDeviceFormat format;
  std::function<bool()> still_wanted;
  uint64_t id = 0;

  mutable std::mutex io_mu;
  std::unique_ptr<IAudioEndpoint> endpoint;
  std::string open_error;

  /** Device thread only (or inline after Shutdown). */
  void OpenOnDevice(IAudioDeviceBackend& backend) {
    std::string error;
    auto opened = backend.Open(kind, format, still_wanted ? still_wanted : [] { return true; }, &error);
    if (!opened) {
      ArbiterLog().info << MediaDeviceKindName(kind) << " open for " << holder << " gave no device: " << error;
    }
    std::lock_guard lock(io_mu);
    endpoint = std::move(opened);
    open_error = endpoint ? std::string() : error;
  }

  std::unique_ptr<IAudioEndpoint> TakeEndpoint() {
    std::lock_guard lock(io_mu);
    return std::move(endpoint);
  }
};

AudioDeviceLease::AudioDeviceLease(std::shared_ptr<State> state) : state_(std::move(state)) {}

AudioDeviceLease::~AudioDeviceLease() {
  // Unhold now: an exclusive re-acquire right after release (session rebuild) must not be refused
  // while the close is still queued. FIFO keeps that close ahead of the next open.
  auto endpoint = std::shared_ptr<IAudioEndpoint>(state_->TakeEndpoint());
  auto core = state_->core.lock();
  if (!core) {
    return;  // endpoint closes here, inline
  }
  core->Unhold(state_->id);
  if (endpoint) {
    core->Post([endpoint = std::move(endpoint)]() mutable { endpoint.reset(); });
  }
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

Roe<void> AudioDeviceLease::Reopen() {
  auto core = state_->core.lock();
  if (!core) {
    return Error("media device arbiter gone");
  }
  auto state = state_;
  core->RunOnDevice([state, core]() {
    // Readers / writers see no device from here until the new endpoint is installed.
    state->TakeEndpoint().reset();
    if (const auto settle = core->backend->ReopenSettle(state->kind); settle.count() > 0) {
      std::this_thread::sleep_for(settle);
    }
    state->OpenOnDevice(*core->backend);
  });
  return {};
}

// ---------------------------------------------------------------------------------------------

MediaDeviceArbiter::MediaDeviceArbiter(std::unique_ptr<IAudioDeviceBackend> backend, MediaDeviceSharePolicy policy)
    : core_(std::make_shared<Core>()) {
  core_->backend = backend ? std::move(backend) : CreateNullAudioDeviceBackend();
  core_->policy = policy;
  Core* core = core_.get();
  core_->thread = std::thread([core]() { core->Run(); });
  core_->thread_id = core_->thread.get_id();
}

MediaDeviceArbiter::~MediaDeviceArbiter() {
  (void)Shutdown(std::chrono::milliseconds::max());
}

namespace {
std::once_flag g_default_once;
std::atomic<MediaDeviceArbiter*> g_default{nullptr};
} // namespace

MediaDeviceArbiter& MediaDeviceArbiter::Default() {
  // Never destroyed: leases may be released during static destruction; ShutdownDefault stops the thread.
  std::call_once(g_default_once, []() {
    g_default.store(new MediaDeviceArbiter(CreateSdlAudioDeviceBackend()), std::memory_order_release);
  });
  return *g_default.load(std::memory_order_acquire);
}

bool MediaDeviceArbiter::ShutdownDefault(std::chrono::milliseconds budget) {
  MediaDeviceArbiter* instance = g_default.load(std::memory_order_acquire);
  return instance ? instance->Shutdown(budget) : true;
}

Roe<std::unique_ptr<AudioDeviceLease>> MediaDeviceArbiter::AcquireAudio(const AudioLeaseRequest& request) {
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
  core_->RunOnDevice([state, core]() { state->OpenOnDevice(*core->backend); });
  return std::unique_ptr<AudioDeviceLease>(new AudioDeviceLease(std::move(state)));
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

bool MediaDeviceArbiter::Shutdown(std::chrono::milliseconds budget) {
  {
    std::lock_guard lock(core_->mu);
    core_->stopping = true;
    core_->cv.notify_all();
  }
  if (!core_->thread.joinable()) {
    return true;
  }
  if (budget != std::chrono::milliseconds::max()) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!core_->finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!core_->finished.load(std::memory_order_acquire)) {
      ArbiterLog().warning << "device thread still closing after " << budget.count()
                           << "ms — detaching (process exit must follow)";
      core_->thread.detach();
      return false;
    }
  }
  core_->thread.join();
  return true;
}

} // namespace pbr
