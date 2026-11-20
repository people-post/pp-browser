#pragma once

#include "lib/ui/QrSvg.h"

#include <string>

namespace ui {
class Context;
}

namespace pbr {

/** How big one of our QR codes is drawn, from the page it is on. */
struct QrDisplay {
  /** Side in dp: phones as large as the page's width and height allow (QrPhoneDisplayDp); desktop the 200dp the style always had. */
  int side_dp = 200;
  /** Side in device pixels, for the `qr.shown` line. */
  int side_px = 200;
  /** `side_dp` as CSS ("320dp"), bound to the code's width and height. */
  std::string css = "200dp";
};

inline constexpr int kQrDesktopDp = 200;

/** Pure part: the display for a page `width_dp` wide and `height_dp` tall at `dp_ratio` pixels per dp. */
QrDisplay QrDisplayFor(bool compact, float width_dp, float height_dp, float dp_ratio);

/** The display on `context`'s window (a null context counts as a 360dp phone / desktop default). */
QrDisplay QrDisplayFor(bool compact, ui::Context* context);

/** Writes one `qr.shown` metrics line: the code's version, modules, error correction level, text bytes and pixels. */
void ReportQrShown(const QrShape& shape, const QrDisplay& display);

} // namespace pbr
