#include "lib/media/QrScanner.h"

#include "lib/media/QrDecode.h"
#include "lib/media/CameraCaptureOrientation.h"
#include "lib/media/VideoYuv.h"
#include "common/Metrics.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
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

using ScanClock = std::chrono::steady_clock;
int64_t ElapsedMs(ScanClock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(ScanClock::now() - since).count();
}

VideoFrameRgba ScanPreview(VideoFrameRgba frame, float zoom, int max_edge = 640) {
  if (zoom <= 1.f && std::max(frame.width, frame.height) <= max_edge) {
    return frame;
  }
  const int crop_w = std::max(2, static_cast<int>(frame.width / zoom));
  const int crop_h = std::max(2, static_cast<int>(frame.height / zoom));
  const float scale = std::min(1.f, max_edge / static_cast<float>(std::max(crop_w, crop_h)));
  VideoFrameRgba out;
  out.width = std::max(2, static_cast<int>(crop_w * scale));
  out.height = std::max(2, static_cast<int>(crop_h * scale));
  out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
  for (int y = 0; y < out.height; ++y) {
    const int sy = (frame.height - crop_h) / 2 + y * crop_h / out.height;
    for (int x = 0; x < out.width; ++x) {
      const int sx = (frame.width - crop_w) / 2 + x * crop_w / out.width;
      const size_t source = (static_cast<size_t>(sy) * frame.width + sx) * 4;
      const size_t target = (static_cast<size_t>(y) * out.width + x) * 4;
      std::copy_n(frame.rgba.data() + source, 4, out.rgba.data() + target);
    }
  }
  return out;
}

class ScanFrameProcessor {
public:
  ScanFrameProcessor(CameraDeviceLease& camera, int rotation) : camera_(camera), rotation_(rotation) {}
  QrScanner::Update Process(VideoFrameRgba frame, ScanClock::time_point now) {
    frame = Upright(std::move(frame), rotation_);
    width = frame.width;
    height = frame.height;
    if (width <= 0 || height <= 0) { return {}; }
    decode_ms = 0;
    detected = false;
    std::optional<std::string> text;
    if (now >= next_decode_) {
      const auto started = ScanClock::now();
      auto result = software_zoom_ > 1.f ? InspectQr(ScanPreview(frame, software_zoom_, 1280)) : InspectQr(frame);
      decode_ms = ElapsedMs(started);
      detected = result.region.has_value();
      text = std::move(result.text);
      if (result.region) {
        last_region_ = now;
        const auto& region = *result.region;
        const float fraction = std::max(region.width, region.height);
        if (!text && fraction > .02f && fraction < .45f &&
            std::abs(region.center_x - .5f) < .25f && std::abs(region.center_y - .5f) < .25f) {
          SetZoom(std::clamp(zoom * .5f / fraction, zoom, 3.f));
        }
      } else if (zoom > 1.f && now - last_region_ > std::chrono::seconds(2)) {
        SetZoom(1.f);
      }
      next_decode_ = now + std::chrono::milliseconds(200);
    }
    if (text) {
      MetricsLine("scan.result").Add("decoded", true).Emit();
    }
    return {ScanPreview(std::move(frame), software_zoom_), std::move(text), {}};
  }
  int width = 0, height = 0;
  int64_t decode_ms = 0;
  bool detected = false;
  float zoom = 1.f;
private:
  void SetZoom(float target) {
    if (std::abs(target - zoom) < .1f) { return; }
    const bool native = camera_.SetZoom(target);
    software_zoom_ = native ? 1.f : target;
    zoom = target;
    MetricsLine("scan.zoom").Add("factor", static_cast<double>(zoom)).Add("native", native).Emit();
  }
  CameraDeviceLease& camera_;
  int rotation_;
  float software_zoom_ = 1.f;
  ScanClock::time_point next_decode_{};
  ScanClock::time_point last_region_{};
};

struct ScanCaptureStats {
  ScanClock::time_point report_at = ScanClock::now();
  int frames = 0, polls = 0, candidates = 0;
  int64_t capture_max_ms = 0, decode_max_ms = 0;
  void Poll(ScanClock::time_point started) {
    ++polls;
    capture_max_ms = std::max(capture_max_ms, ElapsedMs(started));
  }
  void Frame(ScanClock::time_point now, const ScanFrameProcessor& processor) {
    ++frames;
    candidates += processor.detected;
    decode_max_ms = std::max(decode_max_ms, processor.decode_ms);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - report_at).count();
    if (elapsed_ms < 1000) { return; }
    MetricsLine("scan.capture").Add("frames", frames).Add("elapsed_ms", static_cast<int64_t>(elapsed_ms))
        .Add("polls", polls).Add("capture_max_ms", capture_max_ms).Add("width", processor.width)
        .Add("height", processor.height).Add("decode_max_ms", decode_max_ms).Add("candidates", candidates).Emit();
    *this = {};
  }
};
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
    request.format.fps = 30;
    request.format.capture_width = 1280;
    request.format.capture_height = 720;
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
    ScanFrameProcessor processor(*lease, rotate);
    ScanCaptureStats stats;
    while (Current(epoch)) {
      const auto capture_started = std::chrono::steady_clock::now();
      if (auto error = lease->CaptureError(); !error.empty()) {
        Publish(epoch, Update{{}, {}, std::move(error)}, true);
        return;
      }
      auto frame = lease->NextFrame();
      stats.Poll(capture_started);
      if (frame) {
        last_frame = std::chrono::steady_clock::now();
        auto next = processor.Process(std::move(*frame), last_frame);
        stats.Frame(last_frame, processor);
        const bool complete = next.text.has_value();
        Publish(epoch, std::move(next), complete);
        if (complete) {
          return;
        }
      } else if (std::chrono::steady_clock::now() - last_frame > std::chrono::seconds(15)) {
        Publish(epoch, Update{{}, {}, "Camera produced no frames; check camera permission and availability"}, true);
        return;
      }
      std::unique_lock lock(mutex);
      cv.wait_for(lock, std::chrono::milliseconds(5), [&] {
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
