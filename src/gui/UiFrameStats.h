#pragma once

#include <atomic>
#include <cstdint>

namespace pbr {

/** What the UI loop spent since the last TakeAndReset (perf.threads, CallController). */
struct UiFrameTotals {
  int64_t frames = 0;
  int64_t layout_us = 0;
  int64_t render_us = 0;
};

/** Process-wide frame counters: the main loop adds, the call metrics read and reset. Both run on the UI thread. */
class UiFrameStats {
public:
  static void AddFrame(const int64_t layout_us, const int64_t render_us) {
    Frames().fetch_add(1, std::memory_order_relaxed);
    LayoutUs().fetch_add(layout_us, std::memory_order_relaxed);
    RenderUs().fetch_add(render_us, std::memory_order_relaxed);
  }

  static UiFrameTotals TakeAndReset() {
    return {Frames().exchange(0, std::memory_order_relaxed), LayoutUs().exchange(0, std::memory_order_relaxed),
            RenderUs().exchange(0, std::memory_order_relaxed)};
  }

private:
  static std::atomic<int64_t>& Frames() {
    static std::atomic<int64_t> v{0};
    return v;
  }
  static std::atomic<int64_t>& LayoutUs() {
    static std::atomic<int64_t> v{0};
    return v;
  }
  static std::atomic<int64_t>& RenderUs() {
    static std::atomic<int64_t> v{0};
    return v;
  }
};

} // namespace pbr
