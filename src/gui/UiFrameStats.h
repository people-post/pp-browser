#pragma once

#include "gui/CallChromeSync.h"

#include <atomic>
#include <chrono>
#include <cstdint>

namespace pbr {

/** What the UI loop spent since the last TakeAndReset (perf.threads, CallController). */
struct UiFrameTotals {
  int64_t frames = 0;
  int64_t layout_us = 0;
  int64_t render_us = 0;
};

/** Where the UI loop's time and the call's per-frame work went in one perf.threads window (all cumulative). */
struct UiCostTotals {
  int64_t call_tick_us = 0;
  int64_t chat_update_us = 0;
  int64_t shell_update_us = 0;
  int64_t ctx_update_us = 0;
  int64_t roster_us = 0; // inside call_tick_us
  int64_t tiles_us = 0;  // inside call_tick_us
  int64_t roster_builds = 0;
  int64_t icon_lookups = 0;
  int64_t frame_copies = 0;       // frames copied out of the engine (only new ones since CallVideoTiles.h)
  int64_t frame_copies_stale = 0; // copies whose sequence number was the one shown last time (now none expected)
  int64_t video_wakes = 0;
  // ApplyCallChromeSnapshot's closing RequestForceFrame, by the kind of update it pushed.
  int64_t chrome_force_frames_remount = 0;
  int64_t chrome_force_frames_dirty = 0;
  int64_t chrome_force_frames_parts = 0;
  int64_t chrome_force_frames_levels = 0;
  int64_t ui_tasks = 0; // main-loop turns that had posted UI tasks to run (RunUITasks reports no count)
  int64_t loops = 0;    // main-loop turns (frames counts the drawn ones)
};

/**
 * The cost counters of a call's frame work. Adds come from the UI thread, except AddVideoWake (a decoder's or the
 * camera's thread); TakeAndReset reads and clears them on the UI thread. Instances exist for tests; the app uses
 * Instance().
 */
class UiCostCounters {
public:
  void AddCallTickUs(const int64_t us) { call_tick_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddChatUpdateUs(const int64_t us) { chat_update_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddShellUpdateUs(const int64_t us) { shell_update_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddCtxUpdateUs(const int64_t us) { ctx_update_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddRosterUs(const int64_t us) { roster_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddTilesUs(const int64_t us) { tiles_us_.fetch_add(us, std::memory_order_relaxed); }
  void AddRosterBuild() { roster_builds_.fetch_add(1, std::memory_order_relaxed); }
  void AddIconLookup() { icon_lookups_.fetch_add(1, std::memory_order_relaxed); }
  void AddFrameCopy(const bool stale) {
    frame_copies_.fetch_add(1, std::memory_order_relaxed);
    if (stale) {
      frame_copies_stale_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void AddVideoWake() { video_wakes_.fetch_add(1, std::memory_order_relaxed); }
  void AddChromeForceFrame(const CallChromeUpdate update) {
    switch (update) {
      case CallChromeUpdate::Remount:
        chrome_force_frames_remount_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CallChromeUpdate::DirtyOnly:
        chrome_force_frames_dirty_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CallChromeUpdate::PartsOnly:
        chrome_force_frames_parts_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CallChromeUpdate::LevelsOnly:
        chrome_force_frames_levels_.fetch_add(1, std::memory_order_relaxed);
        break;
      case CallChromeUpdate::None:
        break; // never pushed (CallController drops it before the shell)
    }
  }
  void AddUiTaskTurn() { ui_tasks_.fetch_add(1, std::memory_order_relaxed); }
  void AddLoop() { loops_.fetch_add(1, std::memory_order_relaxed); }

  UiCostTotals TakeAndReset() {
    UiCostTotals t;
    t.call_tick_us = call_tick_us_.exchange(0, std::memory_order_relaxed);
    t.chat_update_us = chat_update_us_.exchange(0, std::memory_order_relaxed);
    t.shell_update_us = shell_update_us_.exchange(0, std::memory_order_relaxed);
    t.ctx_update_us = ctx_update_us_.exchange(0, std::memory_order_relaxed);
    t.roster_us = roster_us_.exchange(0, std::memory_order_relaxed);
    t.tiles_us = tiles_us_.exchange(0, std::memory_order_relaxed);
    t.roster_builds = roster_builds_.exchange(0, std::memory_order_relaxed);
    t.icon_lookups = icon_lookups_.exchange(0, std::memory_order_relaxed);
    t.frame_copies = frame_copies_.exchange(0, std::memory_order_relaxed);
    t.frame_copies_stale = frame_copies_stale_.exchange(0, std::memory_order_relaxed);
    t.video_wakes = video_wakes_.exchange(0, std::memory_order_relaxed);
    t.chrome_force_frames_remount = chrome_force_frames_remount_.exchange(0, std::memory_order_relaxed);
    t.chrome_force_frames_dirty = chrome_force_frames_dirty_.exchange(0, std::memory_order_relaxed);
    t.chrome_force_frames_parts = chrome_force_frames_parts_.exchange(0, std::memory_order_relaxed);
    t.chrome_force_frames_levels = chrome_force_frames_levels_.exchange(0, std::memory_order_relaxed);
    t.ui_tasks = ui_tasks_.exchange(0, std::memory_order_relaxed);
    t.loops = loops_.exchange(0, std::memory_order_relaxed);
    return t;
  }

  static UiCostCounters& Instance() {
    static UiCostCounters v;
    return v;
  }

private:
  std::atomic<int64_t> call_tick_us_{0};
  std::atomic<int64_t> chat_update_us_{0};
  std::atomic<int64_t> shell_update_us_{0};
  std::atomic<int64_t> ctx_update_us_{0};
  std::atomic<int64_t> roster_us_{0};
  std::atomic<int64_t> tiles_us_{0};
  std::atomic<int64_t> roster_builds_{0};
  std::atomic<int64_t> icon_lookups_{0};
  std::atomic<int64_t> frame_copies_{0};
  std::atomic<int64_t> frame_copies_stale_{0};
  std::atomic<int64_t> video_wakes_{0};
  std::atomic<int64_t> chrome_force_frames_remount_{0};
  std::atomic<int64_t> chrome_force_frames_dirty_{0};
  std::atomic<int64_t> chrome_force_frames_parts_{0};
  std::atomic<int64_t> chrome_force_frames_levels_{0};
  std::atomic<int64_t> ui_tasks_{0};
  std::atomic<int64_t> loops_{0};
};

/** Adds the microseconds between construction and destruction to one counter (an Add...Us member). */
class UiCostTimer {
public:
  using Add = void (UiCostCounters::*)(int64_t);
  explicit UiCostTimer(const Add add) : add_(add), start_(std::chrono::steady_clock::now()) {}
  UiCostTimer(const UiCostTimer&) = delete;
  UiCostTimer& operator=(const UiCostTimer&) = delete;
  ~UiCostTimer() {
    (UiCostCounters::Instance().*add_)(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start_).count());
  }

private:
  Add add_;
  std::chrono::steady_clock::time_point start_;
};

/** Process-wide frame counters: the main loop adds, the call metrics read and reset. Both run on the UI thread. */
class UiFrameStats {
public:
  static void AddFrame(const int64_t layout_us, const int64_t render_us) {
    Frames().fetch_add(1, std::memory_order_relaxed);
    DrawnFrameCount().fetch_add(1, std::memory_order_relaxed);
    LayoutUs().fetch_add(layout_us, std::memory_order_relaxed);
    RenderUs().fetch_add(render_us, std::memory_order_relaxed);
  }

  /** A frame of 50 ms or more (the main loop's ui.slow_frame threshold), each one, the throttled lines' skipped too. */
  static void AddSlowFrame() { SlowFrameCount().fetch_add(1, std::memory_order_relaxed); }
  /** Slow frames since the start (never reset: a reader takes the difference, as chat.previews does). */
  static int64_t SlowFrames() { return SlowFrameCount().load(std::memory_order_relaxed); }
  /** Frames drawn since the start (never reset: a reader takes the difference, as chat.window's frames_before_fix). */
  static int64_t DrawnFrames() { return DrawnFrameCount().load(std::memory_order_relaxed); }

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
  static std::atomic<int64_t>& SlowFrameCount() {
    static std::atomic<int64_t> v{0};
    return v;
  }
  static std::atomic<int64_t>& DrawnFrameCount() {
    static std::atomic<int64_t> v{0};
    return v;
  }
  static std::atomic<int64_t>& RenderUs() {
    static std::atomic<int64_t> v{0};
    return v;
  }
};

} // namespace pbr
