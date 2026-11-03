#pragma once

#include "common/Module.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/ui/FrameTimeStats.h"

#include <ui/base/Types.h>

#include <chrono>
#include <vector>
#include "common/PbrCompat.h"

namespace ui {
class Context;
}

namespace pbr {

/**
 * Developer-only "Engine test" page (kDevToolsBuild): translucent bars, a keyframe animation and a long
 * list of shadowed, clipped bubbles, plus per-frame timing while the page is open. Decides whether the
 * app can use translucent bars on real phones. See assets/views/ui_perf_test.rml.
 */
class UiPerfTestController : public Module {
public:
  struct Row {
    ui::String line1;
    ui::String line2;
    bool mine = false;
  };

  static constexpr int kRowCount = 2000;
  static constexpr std::chrono::seconds kWindow{10};

  UiPerfTestController();
  ~UiPerfTestController() override = default;

  /** `--ui-perf-test`: open the page once the first frame is up (desktop). */
  static void RequestOpenAtStartup();
  static bool OpenAtStartupRequested();

  void BindShellNavigation(ShellNavigationPorts ports);
  bool RegisterModel(ui::Context* context);

  void Open();
  /** The page's markup is in the document: load its stylesheet. */
  void OnMounted();
  /** The page was dismissed (back button, swipe, Escape). */
  void OnClosed();
  bool IsOpen() const { return open_; }

  /** Call once per presented frame while IsOpen(). */
  void OnFrame(std::chrono::steady_clock::time_point presented);

  /** The `ui.perftest` log line / overlay text for a window. */
  static std::string FormatLogLine(const FrameTimeSummary& s, double refresh_hz, bool translucent, bool anim_on);
  static std::string FormatOverlay(const FrameTimeSummary& s, double refresh_hz);

private:
  void BuildRows();
  void FlushWindow();
  void UpdateOverlay(const FrameTimeSummary& live);
  void ToggleBars();
  void ToggleAnim();
  void Close();

  ui::Context* context_ = nullptr;
  ShellNavigationPorts shell_navigation_;
  bool open_ = false;
  bool bars_translucent_ = true;
  bool anim_on_ = true;
  double refresh_hz_ = 0.0;

  std::vector<Row> rows_;
  ui::String stats_live_;
  ui::String stats_last_;

  FrameTimeStats stats_;
  bool have_prev_present_ = false;
  std::chrono::steady_clock::time_point prev_present_;
  std::chrono::steady_clock::time_point window_start_;
  std::chrono::steady_clock::time_point last_overlay_;
};

} // namespace pbr
