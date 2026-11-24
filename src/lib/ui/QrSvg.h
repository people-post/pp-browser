#pragma once

#include "common/Error.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** The coordinate box QrSvgContent draws in: an `<svg viewBox="0 0 100 100">` shows it whole. */
inline constexpr int kQrSvgViewBox = 100;

/** Side margin, largest and smallest side, in dp, of a QR code drawn on a phone page. */
inline constexpr int kQrPhoneSideMarginDp = 20;
inline constexpr int kQrPhoneMaxDp = 340;
inline constexpr int kQrPhoneMinDp = 200;
/** Height, in dp, the sheet keeps for everything but the code (header, name, hint, two buttons, paddings, safe area). */
inline constexpr int kQrPhoneReservedHeightDp = 400;

/** How the code for a text is built: its version (1..40), modules per side (without the quiet zone) and text bytes. */
struct QrShape {
  int version = 0;
  int modules = 0;
  int bytes = 0;
};

/** The side, in dp, a QR code is drawn at on a phone page `page_width_dp` wide and `page_height_dp` tall: as wide as the
 *  page allows with a 20dp margin each side, at most 340dp, and no taller than leaves room for the name, hint and
 *  buttons (never under 200dp, or the width when that is narrower: a short landscape page scrolls instead). */
int QrPhoneDisplayDp(float page_width_dp, float page_height_dp);

/**
 * A QR code of `text` (byte mode, error correction L for the screen, with its quiet zone) as SVG content for an
 * inline `<svg viewBox="0 0 100 100">`: a white square and the dark modules as one path. Dark on
 * white whatever the theme, which every scanner reads. `shape`, when given, is filled from the same encode.
 */
Roe<std::string> QrSvgContent(const std::string& text, QrShape* shape = nullptr);

/**
 * A QR code of binary `bytes` (byte mode, error correction M, with its quiet zone) as the same SVG content: the link
 * handshake's offer and reply (foundation/crypto/LinkHandshake.h), which no text form would fit as well.
 */
Roe<std::string> QrSvgContentBinary(std::span<const uint8_t> bytes, QrShape* shape = nullptr);

/** The longest edge, in pixels, QrPngBytes aims for. */
inline constexpr int kQrPngTargetPx = 1024;

/**
 * The same text as a PNG (black modules on white, the same quiet zone) for saving or sharing as a picture, with error
 * correction M (it gets recompressed and printed): every module is a whole number of pixels square, as many as fit in
 * about `target_px`, so the edges stay sharp.
 */
Roe<std::vector<uint8_t>> QrPngBytes(const std::string& text, int target_px = kQrPngTargetPx);

} // namespace pbr
