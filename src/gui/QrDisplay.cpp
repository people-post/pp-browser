#include "gui/QrDisplay.h"

#include "common/Metrics.h"
#include "lib/ui/QrSvg.h"

#include <ui/dom/Context.h>

#include <cmath>

namespace pbr {

QrDisplay QrDisplayFor(const bool compact, const float width_dp, const float dp_ratio) {
  QrDisplay display;
  display.side_dp = compact ? QrPhoneDisplayDp(width_dp) : kQrDesktopDp;
  display.side_px = static_cast<int>(std::lround(display.side_dp * (dp_ratio > 0.f ? dp_ratio : 1.f)));
  display.css = std::to_string(display.side_dp) + "dp";
  return display;
}

QrDisplay QrDisplayFor(const bool compact, ui::Context* context) {
  if (context == nullptr) {
    return QrDisplayFor(compact, 360.f, 1.f);
  }
  const float ratio = context->GetDensityIndependentPixelRatio();
  const float width_dp = static_cast<float>(context->GetDimensions().x) / (ratio > 0.f ? ratio : 1.f);
  return QrDisplayFor(compact, width_dp, ratio);
}

void ReportQrShown(const std::string& text, const QrDisplay& display) {
  auto result = QrSvgShape(text);
  if (!result) {
    return;
  }
  const QrShape shape = result.value();
  MetricsLine("qr.shown")
      .Add("version", shape.version)
      .Add("modules", shape.modules)
      .Add("ec", "L")
      .Add("bytes", shape.bytes)
      .Add("px", display.side_px)
      .Emit();
}

} // namespace pbr
