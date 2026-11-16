#pragma once

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** The coordinate box QrSvgContent draws in: an `<svg viewBox="0 0 100 100">` shows it whole. */
inline constexpr int kQrSvgViewBox = 100;

/**
 * A QR code of `text` (byte mode, error correction M, with its quiet zone) as SVG content for an
 * inline `<svg viewBox="0 0 100 100">`: a white square and the dark modules as one path. Dark on
 * white whatever the theme, which every scanner reads.
 */
Roe<std::string> QrSvgContent(const std::string& text);

/** The longest edge, in pixels, QrPngBytes aims for. */
inline constexpr int kQrPngTargetPx = 1024;

/**
 * The same QR code as a PNG (black modules on white, the same quiet zone) for sharing as a picture: every module is
 * a whole number of pixels square, as many as fit in about `target_px`, so the edges stay sharp.
 */
Roe<std::vector<uint8_t>> QrPngBytes(const std::string& text, int target_px = kQrPngTargetPx);

} // namespace pbr
