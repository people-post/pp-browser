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

/**
 * The link handshake's codes (version 29: 133 modules, 141 with the quiet zone) are read by a phone from another
 * screen, so they are drawn as large as the page allows. A desktop gives each module at least 3dp (423dp) when the
 * window has the height for it, and never less than the 280dp the pages always had.
 */
inline constexpr int kLinkQrDesktopMinDp = 280;
inline constexpr int kLinkQrDesktopMaxDp = 423;
/** Height, in dp, the link pages keep for everything but the code on a desktop (title, line, digits, buttons). */
inline constexpr int kLinkQrDesktopReservedHeightDp = 470;
/** Width the desktop page keeps beside the code (card padding and the window's own margin). */
inline constexpr int kLinkQrDesktopReservedWidthDp = 96;

/** Pure part: the link code's display on a page `width_dp` wide and `height_dp` tall (phones: the page's width). */
QrDisplay LinkQrDisplayFor(bool compact, float width_dp, float height_dp, float dp_ratio,
                           int reserved_width_dp = kLinkQrDesktopReservedWidthDp);

/** The link code's display on `context`'s window (a null context counts as a 390dp phone / 1100dp desktop). */
QrDisplay LinkQrDisplayFor(bool compact, ui::Context* context, int reserved_width_dp = kLinkQrDesktopReservedWidthDp);

/** Width a desktop window keeps beside the settings page's own column: the navigation rail and the section list. */
inline constexpr int kLinkQrDesktopSettingsReservedWidthDp = 500;

/**
 * The new device's link page on a desktop computer (ShellState::desktop_computer; an iPad and a phone keep their own
 * page): one card in the window, as large as the window's own height allows and never taller than it. A window wide
 * enough puts the code on the left and the steps, the 4 digits and the buttons on the right; a narrower one stacks them
 * and shrinks the code to what is left of the height. Below the 280dp minimum the card's body scrolls and Back stays
 * in a footer that never does.
 */
struct LinkComputerLayout {
  bool two_columns = false;
  int side_dp = kLinkQrDesktopMinDp;
};

/** Card: 90% of the window wide and tall (air around it), at most 720dp wide, 20dp padding a side, 20dp between columns. */
inline constexpr float kLinkComputerCardFraction = 0.9f;
inline constexpr int kLinkComputerCardMaxWidthDp = 720;
inline constexpr int kLinkComputerPaddingDp = 20;
inline constexpr int kLinkComputerGapDp = 20;
/** The code on this page is at most this large (the main device's page keeps kLinkQrDesktopMaxDp). */
inline constexpr int kLinkComputerQrMaxDp = 320;
/** The right column's least width (the steps and the Paste / Scan row). */
inline constexpr int kLinkComputerRightMinDp = 340;
/** Height, in dp, kept for everything but the code: two columns (title, Copy, footer, padding), one column (all that). */
inline constexpr int kLinkComputerReservedTwoColumnsHeightDp = 190;
inline constexpr int kLinkComputerReservedOneColumnHeightDp = 490;
/** Two columns need at least this much window height (the right column's own height, measured by the layout test). */
inline constexpr int kLinkComputerTwoColumnsMinHeightDp = 470;

/** Pure part: the layout for a window `width_dp` by `height_dp`. */
LinkComputerLayout LinkComputerLayoutFor(float width_dp, float height_dp);

/** The same for `context`'s window (a null context counts as 1100 x 800). */
LinkComputerLayout LinkComputerLayoutFor(ui::Context* context);

/** Pure part: the display for a page `width_dp` wide and `height_dp` tall at `dp_ratio` pixels per dp. */
QrDisplay QrDisplayFor(bool compact, float width_dp, float height_dp, float dp_ratio);

/** The display on `context`'s window (a null context counts as a 360dp phone / desktop default). */
QrDisplay QrDisplayFor(bool compact, ui::Context* context);

/** Writes one `qr.shown` metrics line: the code's version, modules, error correction level, text bytes and pixels. */
void ReportQrShown(const QrShape& shape, const QrDisplay& display);

} // namespace pbr
