#include "lib/scan/QrScanner.h"

#include "lib/scan/QrDecode.h"
#include "lib/scan/QrScanPolicy.h"
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
constexpr int kQrCaptureWidth = 1920;
constexpr int kQrCaptureHeight = 1080;
// Reference implementation: RotateRgba90Cw (VideoYuv.cpp); keep the two in sync.
// One pass and one allocation for any multiple of 90 degrees clockwise (was one full-frame copy per turn).
VideoFrameRgba Upright(VideoFrameRgba frame, int rotation) {
  const int turns = ((rotation % 360 + 360) % 360) / 90;
  if (turns == 0) {
    return frame;
  }
  if (frame.width <= 0 || frame.height <= 0 ||
      frame.rgba.size() < static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 4) {
    return {};
  }
  VideoFrameRgba out;
  out.width = turns == 2 ? frame.width : frame.height;
  out.height = turns == 2 ? frame.height : frame.width;
  out.rgba.resize(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4);
  const size_t in_w = static_cast<size_t>(frame.width);
  for (int y = 0; y < out.height; ++y) {
    for (int x = 0; x < out.width; ++x) {
      // Source pixel of the clockwise rotation: 90 -> (y, H-1-x), 180 -> (W-1-x, H-1-y), 270 -> (W-1-y, x).
      const size_t sx = turns == 1 ? y : turns == 2 ? frame.width - 1 - x : frame.width - 1 - y;
      const size_t sy = turns == 1 ? frame.height - 1 - x : turns == 2 ? frame.height - 1 - y : x;
      std::copy_n(frame.rgba.data() + (sy * in_w + sx) * 4, 4,
                  out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4);
    }
  }
  return out;
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
  ScanFrameProcessor(CameraDeviceLease& camera, int rotation, ScanClock::time_point scan_started)
      : camera_(camera), rotation_(rotation) {
    policy_.Begin(scan_started);
  }
  bool DecodeDue(ScanClock::time_point now) const { return policy_.DecodeDue(now); }
  // `zoom_state` must be sampled before `frame` was fetched: a frame captured mid-ramp judged
  // against the settled state would compound a second zoom on top of the first (Windows CI).
  QrScanner::Update Process(VideoFrameRgba frame, ScanClock::time_point now, const CameraZoomState& zoom_state) {
    const bool swap = ((rotation_ % 360 + 360) % 360) / 90 % 2 == 1;
    width = swap ? frame.height : frame.width;
    height = swap ? frame.width : frame.height;
    if (width < 2 || height < 2) {
      return {};
    }
    decode_ms = 0;
    detected = false;
    if (!policy_.DecodeDue(now)) {
      // Most frames only feed the preview: shrink to the preview size first, then rotate the small copy.
      auto preview = Upright(ScanPreview(std::move(frame), software_zoom_), rotation_);
      if (preview.width < 2 || preview.height < 2) {
        return {};
      }
      return {std::move(preview), std::nullopt, {}};
    }
    frame = Upright(std::move(frame), rotation_);
    if (frame.width < 2 || frame.height < 2) {
      return {};
    }
    std::optional<std::string> text;
    {
      const auto started = ScanClock::now();
      auto result =
          software_zoom_ > 1.f ? InspectQr(ScanPreview(frame, software_zoom_, kQrCaptureWidth)) : InspectQr(frame);
      const auto finished = ScanClock::now();
      decode_ms = ElapsedMs(started);
      detected = result.region.has_value();
      text = std::move(result.text);
      policy_.AfterDecode(started, finished, detected);
      if (result.region) {
        const auto& region = *result.region;
        const float fraction = std::max(region.width, region.height);
        if (!text && fraction > .02f && fraction < .45f && std::abs(region.center_x - .5f) < .25f &&
            std::abs(region.center_y - .5f) < .25f) {
          const auto& state = zoom_state;
          // Native zoom is centered: retain the whole candidate and its quiet-zone margin.
          const float extent = std::max(std::abs(region.center_x - .5f) + region.width * .7f,
                                        std::abs(region.center_y - .5f) + region.height * .7f);
          if (!native_zoom_ || (!state.ramping && std::abs(state.factor - zoom) < .05f)) {
            const float base = native_zoom_ ? state.factor : zoom;
            const float target = std::min(base * .5f / fraction, base * .5f / extent);
            SetZoom(std::clamp(target, base, 3.f), now);
            candidate_zoom_ = zoom > 1.f;
          }
        }
      } else if (candidate_zoom_ && policy_.ShouldResetCandidateZoom(now)) {
        SetZoom(1.f, now);
        candidate_zoom_ = false;
      } else if (!candidate_zoom_) {
        Hunt(now, zoom_state);
      }
    }
    if (text) {
      MetricsLine("scan.result")
          .Add("decoded", true)
          .Add("elapsed_ms", policy_.ElapsedMs(now))
          .Add("zoom_changes", policy_.ZoomChanges())
          .Emit();
    }
    return {ScanPreview(std::move(frame), software_zoom_), std::move(text), {}};
  }
  int width = 0, height = 0;
  int64_t decode_ms = 0;
  bool detected = false;
  float zoom = 1.f;

private:
  // A code too far away yields no candidate at all (a capstone needs about three pixels per
  // module), so nothing would ever trigger the candidate zoom. Once the zoom has settled with
  // nothing in view, QrScanPolicy lets it swing 1x <-> 2x now and then (never in a scan's first
  // seconds, never close behind another zoom change); a candidate then takes over.
  void Hunt(ScanClock::time_point now, const CameraZoomState& state) {
    const bool settled = !state.supported || !native_zoom_ || (!state.ramping && std::abs(state.factor - zoom) < .05f);
    if (!settled) {
      return;
    }
    if (const auto target = policy_.ProbeTarget(now, zoom)) {
      SetZoom(*target, now, true);
    }
  }
  void SetZoom(float target, ScanClock::time_point now, bool hunt = false) {
    const auto state = camera_.ZoomState();
    if (state.supported) {
      target = std::clamp(target, 1.f, state.maximum);
    }
    if (std::abs(target - zoom) < .1f) {
      return;
    }
    const bool native = camera_.SetZoom(target);
    native_zoom_ = native;
    software_zoom_ = native ? 1.f : target;
    zoom = target;
    policy_.OnZoomChanged(now);
    MetricsLine("scan.zoom").Add("factor", static_cast<double>(zoom)).Add("native", native).Add("hunt", hunt).Emit();
  }
  CameraDeviceLease& camera_;
  int rotation_;
  float software_zoom_ = 1.f;
  bool native_zoom_ = false;
  bool candidate_zoom_ = false;
  QrScanPolicy policy_;
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
    if (elapsed_ms < 1000) {
      return;
    }
    MetricsLine("scan.capture")
        .Add("frames", frames)
        .Add("elapsed_ms", static_cast<int64_t>(elapsed_ms))
        .Add("polls", polls)
        .Add("capture_max_ms", capture_max_ms)
        .Add("width", processor.width)
        .Add("height", processor.height)
        .Add("decode_max_ms", decode_max_ms)
        .Add("candidates", candidates)
        .Emit();
    *this = {};
  }
};
} // namespace

struct QrScanner::State {
  State(MediaDeviceArbiter& arbiter, std::chrono::milliseconds timeout) : devices(arbiter), no_frame_timeout(timeout) {}
  MediaDeviceArbiter& devices;
  const std::chrono::milliseconds no_frame_timeout;
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
    const auto scan_started = ScanClock::now();
    CameraLeaseRequest request;
    request.holder = "qr-scanner";
    request.format.display_rotation_deg = display_rotation;
    request.format.fps = 30;
    // 1080p: the link handshake's codes are version 29 (141 modules with the quiet zone); filling 60% of a 720-line
    // frame gives them barely 3 pixels per module, quirc's floor. A camera without 1080p delivers the closest it has.
    request.format.capture_width = kQrCaptureWidth;
    request.format.capture_height = kQrCaptureHeight;
    request.format.prefer_front_facing = false;
    if (!Current(epoch)) {
      return;
    }
    auto acquired = devices.AcquireCamera(request);
    // Busy (a call still letting it go) or not responding yet: wait for it while the scan is wanted
    // (L016), as long as a silent camera may take.
    const auto give_up = ScanClock::now() + no_frame_timeout;
    while (!acquired && Current(epoch) && ScanClock::now() < give_up && !devices.CanAcquire(MediaDeviceKind::Camera)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (devices.CanAcquire(MediaDeviceKind::Camera)) {
        acquired = devices.AcquireCamera(request);
      }
    }
    if (!acquired) {
      // Someone else holds the camera (a call), or it is not responding / absent.
      const bool busy = !devices.Holders(MediaDeviceKind::Camera).empty();
      Publish(epoch, Update{{}, {}, acquired.error().message, busy ? ErrorKind::CameraBusy : ErrorKind::Other}, true);
      return;
    }
    auto lease = std::move(*acquired);
    const auto geometry = lease->Geometry();
    CameraCaptureTransform transform;
    transform.rotate_cw = geometry.rotate_cw;
    transform.front_facing = geometry.front_facing;
    const int rotate = CameraPreviewRotateCw(transform);
    auto last_frame = std::chrono::steady_clock::now();
    ScanFrameProcessor processor(*lease, rotate, scan_started);
    ScanCaptureStats stats;
    // Wake on the frame cadence, not every few ms: each wake costs three locked camera calls.
    const auto frame_period = std::chrono::milliseconds(request.format.fps > 0 ? 1000 / request.format.fps : 16);
    while (Current(epoch)) {
      const auto capture_started = std::chrono::steady_clock::now();
      if (auto error = lease->CaptureError(); !error.empty()) {
        // Only an OS refusal sends the user to Settings; "no camera behind the lease" is not one.
        const ErrorKind kind = lease->PermissionDenied() ? ErrorKind::PermissionDenied : ErrorKind::Other;
        Publish(epoch, Update{{}, {}, std::move(error), kind}, true);
        return;
      }
      const auto zoom_state = processor.DecodeDue(capture_started) ? lease->ZoomState() : CameraZoomState{};
      auto frame = lease->NextFrame();
      stats.Poll(capture_started);
      auto idle_wait = frame_period / 2;
      if (frame) {
        last_frame = capture_started;
        // Capped: a jittery camera must not make the next poll land just before its frame and then sleep half a period.
        idle_wait = std::min(frame_period, std::chrono::milliseconds(16));
        auto next = processor.Process(std::move(*frame), last_frame, zoom_state);
        stats.Frame(last_frame, processor);
        const bool complete = next.text.has_value();
        Publish(epoch, std::move(next), complete);
        if (complete) {
          return;
        }
      } else if (lease->PermissionPending()) {
        // The OS permission dialog is up: frames cannot arrive yet, so the no-frame clock only
        // starts once the user has answered.
        last_frame = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() - last_frame > no_frame_timeout) {
        Publish(epoch,
                Update{{}, {}, "Camera produced no frames; check camera permission and availability", ErrorKind::Other},
                true);
        return;
      }
      std::unique_lock lock(mutex);
      // After a frame the next one is a full period away (less the time spent on this one).
      const auto spent =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - capture_started);
      cv.wait_for(lock, std::max(std::chrono::milliseconds(1), idle_wait - spent),
                  [&] { return closing || !scanning || generation != epoch; });
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

QrScanner::QrScanner(MediaDeviceArbiter& devices, std::chrono::milliseconds no_frame_timeout)
    : state_(std::make_unique<State>(devices, no_frame_timeout)) {
  state_->worker = std::thread([this] { state_->Run(); });
}
QrScanner::~QrScanner() {
  Shutdown();
}

bool QrScanner::CameraAvailable() const {
  return state_->devices.HasCamera();
}

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
