#include "common/chat/MessagingLimits.h"
#include "gui/UiPerfTestController.h"

#include "common/PbrCompat.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/AssetIO.h"
#include "foundation/platform/IAssetLocator.h"
#include "foundation/platform/MobileWindowSizing.h"
#include "foundation/product/DevTools.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/RmlMount.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/ViewCatalog.h"

#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace pbr {
namespace {

std::atomic<bool> g_open_at_startup{false};

/** Gaps above this are the app being backgrounded / suspended, not a slow frame. With the animation on
 * (or while scrolling) frames are continuous, so every shorter gap is a real frame time. */
constexpr double kIdleGapMs = 30000.0;

} // namespace

UiPerfTestController::UiPerfTestController() {
  redirectLogger("Ui.PerfTest");
}

void UiPerfTestController::RequestOpenAtStartup() {
  g_open_at_startup.store(true);
}

bool UiPerfTestController::OpenAtStartupRequested() {
  return kDevToolsBuild && g_open_at_startup.load();
}

void UiPerfTestController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

bool UiPerfTestController::RegisterModel(ui::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;
  return DataModelHost::Instance().Register(context, "ui_perf", [this](ui::DataModelConstructor& ctor) {
    if (auto row = ctor.RegisterStruct<Row>()) {
      row.RegisterMember("line1", &Row::line1);
      row.RegisterMember("line2", &Row::line2);
      row.RegisterMember("mine", &Row::mine);
    }
    ctor.RegisterArray<std::vector<Row>>();
    ctor.Bind("rows", &rows_);
    ctor.Bind("bars_translucent", &bars_translucent_);
    ctor.Bind("anim_on", &anim_on_);
    ctor.Bind("stress", &stress_);
    ctor.Bind("stats_live", &stats_live_);
    ctor.Bind("stats_last", &stats_last_);
    ctor.BindEventCallback("toggle_bars", DataEvent<>([this]() { ToggleBars(); }));
    ctor.BindEventCallback("toggle_anim", DataEvent<>([this]() { ToggleAnim(); }));
    ctor.BindEventCallback("toggle_stress", DataEvent<>([this]() { ToggleStress(); }));
    ctor.BindEventCallback("close_ui_perf", DataEvent<>([this]() { Close(); }));
  });
}

void UiPerfTestController::BuildRows() {
  const int count = RowCount();
  if (static_cast<int>(rows_.size()) == count) {
    return;
  }
  rows_.clear();
  rows_.reserve(count);
  for (int i = 0; i < count; ++i) {
    Row row;
    row.mine = (i % 3) == 2;
    const std::string n = std::to_string(i + 1);
    row.line1 = ("#" + n + " 会议改到下午三点 moved to 3 pm").c_str();
    row.line2 = "好的，我会发给大家 Sounds good";
    rows_.push_back(std::move(row));
  }
}

void UiPerfTestController::Open() {
  if (open_ || !kDevToolsBuild || !shell_navigation_.push_transient) {
    return;
  }
  stress_ = false;
  BuildRows();
  bars_translucent_ = true;
  anim_on_ = true;
  refresh_hz_ = PrimaryDisplayRefreshHz();
  stats_.Reset();
  have_prev_present_ = false;
  const auto now = std::chrono::steady_clock::now();
  window_start_ = now;
  last_overlay_ = now;
  stats_live_ = Tr("dev.perf.waiting").c_str();
  stats_last_ = "";
  open_ = true;
  DataModelHost::Instance().DirtyAll("ui_perf");

  PaneSpec spec;
  spec.key = "ui_perf_test";
  shell_navigation_.push_transient(spec);
}

void UiPerfTestController::OnMounted() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  const std::string css = ViewCatalog::LoadFile(IAssetLocator::Instance().Resolve("themes/ui_perf_test.rcss"));
  if (css.empty()) {
    log().warning << "themes/ui_perf_test.rcss missing or empty";
    return;
  }
  RmlMount::InjectRcss(context_->GetDocument(0), css, "ui_perf_test");
}

void UiPerfTestController::Close() {
  if (shell_navigation_.pop_transient) {
    shell_navigation_.pop_transient(); // ShellHost then calls OnClosed()
  }
}

void UiPerfTestController::OnClosed() {
  if (!open_) {
    return;
  }
  FlushWindow();
  open_ = false;
}

void UiPerfTestController::OnFrame(std::chrono::steady_clock::time_point presented) {
  if (!open_) {
    return;
  }
  if (have_prev_present_) {
    const double ms = std::chrono::duration<double, std::milli>(presented - prev_present_).count();
    if (ms < kIdleGapMs) {
      stats_.Add(ms);
    }
  }
  prev_present_ = presented;
  have_prev_present_ = true;

  if (presented - window_start_ >= kWindow) {
    FlushWindow();
  } else if (presented - last_overlay_ >= std::chrono::seconds(1)) {
    last_overlay_ = presented;
    UpdateOverlay(stats_.Summarize());
  }
}

std::string UiPerfTestController::FormatLogLine(const FrameTimeSummary& s, double refresh_hz, bool translucent,
                                                bool anim_on, int rows) {
  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "ui.perftest frames=%zu avg_fps=%.1f p99_ms=%.1f max_ms=%.1f over50=%zu refresh_hz=%.0f bars=%s "
                "anim=%s rows=%d",
                s.frames, s.avg_fps, s.p99_ms, s.max_ms, s.over50, refresh_hz, translucent ? "translucent" : "opaque",
                anim_on ? "on" : "off", rows);
  return buf;
}

std::string UiPerfTestController::FormatOverlay(const FrameTimeSummary& s, double refresh_hz) {
  char buf[200];
  std::snprintf(buf, sizeof(buf), "%.1f fps  p99 %.1f ms  max %.1f ms  >50ms: %zu  (%zu frames, %.0f Hz)", s.avg_fps,
                s.p99_ms, s.max_ms, s.over50, s.frames, refresh_hz);
  return buf;
}

void UiPerfTestController::UpdateOverlay(const FrameTimeSummary& live) {
  stats_live_ = FormatOverlay(live, refresh_hz_).c_str();
  DataModelHost::Instance().Dirty("ui_perf", "stats_live");
}

void UiPerfTestController::FlushWindow() {
  const FrameTimeSummary s = stats_.Summarize();
  if (s.frames > 0) {
    log().info << FormatLogLine(s, refresh_hz_, bars_translucent_, anim_on_, RowCount());
    // Pass rule (DEV/PLAN-ui-refresh-impl.md batch 0): avg >= 90% of the refresh rate, p99 <= 33 ms, no frame > 50 ms.
    const bool pass = refresh_hz_ > 0.0 && s.avg_fps >= 0.9 * refresh_hz_ && s.p99_ms <= 33.0 && s.over50 == 0;
    stats_last_ = ((pass ? "PASS  " : "FAIL  ") + FormatOverlay(s, refresh_hz_)).c_str();
    stats_live_ = "";
    DataModelHost::Instance().Dirty("ui_perf", "stats_last");
    DataModelHost::Instance().Dirty("ui_perf", "stats_live");
  }
  stats_.Reset();
  window_start_ = std::chrono::steady_clock::now();
  last_overlay_ = window_start_;
}

void UiPerfTestController::ToggleBars() {
  FlushWindow(); // keep each logged window to one setting
  bars_translucent_ = !bars_translucent_;
  DataModelHost::Instance().Dirty("ui_perf", "bars_translucent");
}

void UiPerfTestController::ToggleAnim() {
  FlushWindow();
  anim_on_ = !anim_on_;
  DataModelHost::Instance().Dirty("ui_perf", "anim_on");
}

int UiPerfTestController::RowCount() const {
  return stress_ ? kStressRowCount : static_cast<int>(kMaxMessagesDomWindow);
}

void UiPerfTestController::ToggleStress() {
  FlushWindow();
  stress_ = !stress_;
  BuildRows();
  DataModelHost::Instance().Dirty("ui_perf", "stress");
  DataModelHost::Instance().Dirty("ui_perf", "rows");
}

} // namespace pbr
