#pragma once

#include "common/Error.h"

#include <string>
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

} // namespace pbr
