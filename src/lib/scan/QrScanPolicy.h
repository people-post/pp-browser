#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

namespace pbr {

/** How a camera scan paces its decoding and zooming. Named so a tuning is one line here. */
inline constexpr std::chrono::milliseconds kQrDecodeInterval{100}; // between decodes when no code is in view
inline constexpr std::chrono::milliseconds kQrCandidateWindow{
    1000};                                                    // finder patterns seen this recently: decode every frame
inline constexpr std::chrono::milliseconds kQrZoomQuiet{500}; // never change zoom this soon after a candidate
inline constexpr std::chrono::milliseconds kQrNoProbeAtStart{4000}; // no zoom probing in the first seconds of a scan
inline constexpr std::chrono::milliseconds kQrProbeInterval{4000};  // at most one probe (or other zoom change) per this
inline constexpr std::chrono::milliseconds kQrCandidateHold{
    5000};                                 // keep a zoom toward a code this long after it was last seen
inline constexpr float kQrProbeZoom = 2.f; // probing swings 1x <-> this, never further

/**
 * The timing rules of a scan, with the clock passed in (no camera, no thread): when to decode, when the camera zoom may
 * move on its own, and when a zoom made toward a seen code is given back. The scanner asks; it does the zooming.
 */
class QrScanPolicy {
public:
  using Time = std::chrono::steady_clock::time_point;

  void Begin(const Time now) {
    *this = QrScanPolicy();
    begin_ = now;
  }

  bool DecodeDue(const Time now) const { return now >= next_decode_; }

  /**
   * A decode of `decode_ms` finished at `now`; `candidate` says finder patterns were in the frame. While a candidate
   * was seen in the last second the next frame is decoded at once, but never more than half the worker's time (the
   * same thread feeds the preview, so a slow decode would otherwise freeze it); else every kQrDecodeInterval.
   */
  void AfterDecode(const Time now, const bool candidate, const int64_t decode_ms) {
    if (candidate) {
      last_candidate_ = now;
    }
    const bool recent = last_candidate_ && now - *last_candidate_ <= kQrCandidateWindow;
    next_decode_ = now + (recent ? std::chrono::milliseconds(std::max<int64_t>(0, decode_ms)) : kQrDecodeInterval);
  }

  /** A candidate (finder patterns) was in view within kQrZoomQuiet: the zoom must not move on its own. */
  bool CandidateRecent(const Time now) const { return last_candidate_ && now - *last_candidate_ < kQrZoomQuiet; }

  /** The zoom a probe should go to now (1x <-> 2x), if one is due. */
  std::optional<float> ProbeTarget(const Time now, const float zoom) const {
    if (now - begin_ < kQrNoProbeAtStart || CandidateRecent(now)) {
      return std::nullopt;
    }
    if (last_zoom_change_ && now - *last_zoom_change_ < kQrProbeInterval) {
      return std::nullopt;
    }
    return zoom < 1.5f ? kQrProbeZoom : 1.f;
  }

  /** A zoom made toward a seen code is due back to 1x: the code has been gone for kQrCandidateHold. */
  bool ShouldResetCandidateZoom(const Time now) const {
    return !CandidateRecent(now) && (!last_candidate_ || now - *last_candidate_ > kQrCandidateHold);
  }

  void OnZoomChanged(const Time now) {
    last_zoom_change_ = now;
    ++zoom_changes_;
  }
  int ZoomChanges() const { return zoom_changes_; }
  int64_t ElapsedMs(const Time now) const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - begin_).count();
  }

private:
  Time begin_{};
  Time next_decode_{};
  std::optional<Time> last_candidate_;
  std::optional<Time> last_zoom_change_;
  int zoom_changes_ = 0;
};

} // namespace pbr
