#include "lib/media/QrScanner.h"

#include "lib/media/QrDecode.h"
#include "lib/media/CameraCaptureOrientation.h"
#include "lib/media/VideoYuv.h"
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace pbr {
namespace {
VideoFrameRgba Upright(VideoFrameRgba frame, int rotation) {
  for (int i = 0; i < ((rotation % 360 + 360) % 360) / 90; ++i) {
    VideoFrameRgba rotated;
    if (!RotateRgba90Cw(frame, rotated)) {
      return {};
    }
    frame = std::move(rotated);
  }
  return frame;
}
} // namespace

struct QrScanner::State {
  explicit State(MediaDeviceArbiter& arbiter) : devices(arbiter) {}
  MediaDeviceArbiter& devices;
  std::mutex mutex;
  std::condition_variable cv;
  std::thread worker;
  bool closing = false;
  bool scanning = false;
  uint64_t generation = 0;
  int rotation = 0;
  Update update;

  bool Current(uint64_t epoch) {
    std::lock_guard lock(mutex);
    return !closing && scanning && generation == epoch;
  }

  void Publish(uint64_t epoch, Update next, bool terminal) {
    std::lock_guard lock(mutex);
    if (closing || !scanning || generation != epoch) {
      return;
    }
    update = std::move(next);
    if (terminal) {
      scanning = false;
    }
  }

  void Capture(uint64_t epoch, int display_rotation) {
    CameraLeaseRequest request;
    request.holder = "qr-scanner";
    request.format.display_rotation_deg = display_rotation;
    request.format.fps = 15;
    request.format.prefer_front_facing = false;
    if (!Current(epoch)) {
      return;
    }
    auto acquired = devices.AcquireCamera(request);
    if (!acquired) {
      Publish(epoch, Update{{}, {}, acquired.error().message}, true);
      return;
    }
    auto lease = std::move(*acquired);
    const auto geometry = lease->Geometry();
    CameraCaptureTransform transform;
    transform.rotate_cw = geometry.rotate_cw;
    transform.front_facing = geometry.front_facing;
    const int rotate = CameraPreviewRotateCw(transform);
    auto last_frame = std::chrono::steady_clock::now();
    while (Current(epoch)) {
      if (auto error = lease->CaptureError(); !error.empty()) {
        Publish(epoch, Update{{}, {}, std::move(error)}, true);
        return;
      }
      if (auto frame = lease->NextFrame()) {
        last_frame = std::chrono::steady_clock::now();
        auto preview = Upright(std::move(*frame), rotate);
        auto text = DecodeQr(preview);
        const bool complete = text.has_value();
        Publish(epoch, Update{std::move(preview), std::move(text), {}}, complete);
        if (complete) {
          return;
        }
      } else if (std::chrono::steady_clock::now() - last_frame > std::chrono::seconds(15)) {
        Publish(epoch, Update{{}, {}, "Camera produced no frames; check camera permission and availability"}, true);
        return;
      }
      std::unique_lock lock(mutex);
      cv.wait_for(lock, std::chrono::milliseconds(90), [&] {
        return closing || !scanning || generation != epoch;
      });
    }
  }

  void Run() {
    for (;;) {
      std::unique_lock lock(mutex);
      cv.wait(lock, [&] { return closing || scanning; });
      if (closing) {
        return;
      }
      const auto epoch = generation;
      const int display_rotation = rotation;
      lock.unlock();
      Capture(epoch, display_rotation);
    }
  }
};

QrScanner::QrScanner(MediaDeviceArbiter& devices) : state_(std::make_unique<State>(devices)) {
  state_->worker = std::thread([this] { state_->Run(); });
}
QrScanner::~QrScanner() { Shutdown(); }

void QrScanner::Start(int display_rotation_deg) {
  std::lock_guard lock(state_->mutex);
  if (state_->closing) {
    return;
  }
  ++state_->generation;
  state_->rotation = display_rotation_deg;
  state_->update = {};
  state_->scanning = true;
  state_->cv.notify_one();
}
void QrScanner::Stop() {
  std::lock_guard lock(state_->mutex);
  ++state_->generation;
  state_->scanning = false;
  state_->update = {};
  state_->cv.notify_one();
}
QrScanner::Update QrScanner::Poll() {
  std::lock_guard lock(state_->mutex);
  return std::exchange(state_->update, {});
}
void QrScanner::Shutdown() {
  {
    std::lock_guard lock(state_->mutex);
    state_->closing = true;
    state_->scanning = false;
    ++state_->generation;
    state_->update = {};
    state_->cv.notify_one();
  }
  if (state_->worker.joinable()) {
    state_->worker.join();
  }
}
} // namespace pbr
