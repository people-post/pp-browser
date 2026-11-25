#include "gui/QrDisplay.h"

#include "common/Metrics.h"
#include "lib/ui/QrSvg.h"

#include <ui/dom/Context.h>

#include <algorithm>
#include <cmath>

namespace pbr {

QrDisplay QrDisplayFor(const bool compact, const float width_dp, const float height_dp, const float dp_ratio) {
  QrDisplay display;
  display.side_dp = compact ? QrPhoneDisplayDp(width_dp, height_dp) : kQrDesktopDp;
  display.side_px = static_cast<int>(std::lround(display.side_dp * (dp_ratio > 0.f ? dp_ratio : 1.f)));
  display.css = std::to_string(display.side_dp) + "dp";
  return display;
}

QrDisplay QrDisplayFor(const bool compact, ui::Context* context) {
  if (context == nullptr) {
    return QrDisplayFor(compact, 360.f, 780.f, 1.f);
  }
  const float ratio = context->GetDensityIndependentPixelRatio();
  const float scale = ratio > 0.f ? ratio : 1.f;
  const float width_dp = static_cast<float>(context->GetDimensions().x) / scale;
  const float height_dp = static_cast<float>(context->GetDimensions().y) / scale;
  return QrDisplayFor(compact, width_dp, height_dp, ratio);
}

QrDisplay LinkQrDisplayFor(const bool compact, const float width_dp, const float height_dp, const float dp_ratio) {
  QrDisplay display;
  if (compact) {
    // The page scrolls, so only its width limits the code: the margins and cap QrPhoneDisplayDp keeps.
    display.side_dp = QrPhoneDisplayDp(width_dp, 100000.f);
  } else {
    const int by_height = static_cast<int>(height_dp) - kLinkQrDesktopReservedHeightDp;
    const int by_width = static_cast<int>(width_dp) - kLinkQrDesktopReservedWidthDp;
    display.side_dp = std::max(kLinkQrDesktopMinDp, std::min({kLinkQrDesktopMaxDp, by_height, by_width}));
  }
  display.side_px = static_cast<int>(std::lround(display.side_dp * (dp_ratio > 0.f ? dp_ratio : 1.f)));
  display.css = std::to_string(display.side_dp) + "dp";
  return display;
}

QrDisplay LinkQrDisplayFor(const bool compact, ui::Context* context) {
  if (context == nullptr) {
    return compact ? LinkQrDisplayFor(true, 390.f, 844.f, 1.f) : LinkQrDisplayFor(false, 1100.f, 800.f, 1.f);
  }
  const float ratio = context->GetDensityIndependentPixelRatio();
  const float scale = ratio > 0.f ? ratio : 1.f;
  return LinkQrDisplayFor(compact, static_cast<float>(context->GetDimensions().x) / scale,
                          static_cast<float>(context->GetDimensions().y) / scale, ratio);
}

void ReportQrShown(const QrShape& shape, const QrDisplay& display) {
  MetricsLine("qr.shown")
      .Add("version", shape.version)
      .Add("modules", shape.modules)
      .Add("ec", "L")
      .Add("bytes", shape.bytes)
      .Add("px", display.side_px)
      .Emit();
}

} // namespace pbr
