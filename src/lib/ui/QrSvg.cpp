#include "lib/ui/QrSvg.h"

#include "foundation/platform/ImageEncode.h"

#include <SDL3/SDL.h>
#include <qrcodegen.hpp>

#include <algorithm>
#include <exception>
#include <sstream>

namespace pbr {

namespace {

/** The blank modules a scanner needs around the code. */
constexpr int kQuietZone = 4;

/**
 * On screen: error correction LOW, the least redundancy, so the fewest modules and the biggest ones for the same drawn
 * size. Every scanner reads any level, and the payload is unchanged, so this is not a format anyone has to agree on.
 */
constexpr qrcodegen::QrCode::Ecc kScreenEcc = qrcodegen::QrCode::Ecc::LOW;

/**
 * The saved / shared picture: MEDIUM, because it passes through photo libraries, chat apps, JPEG recompression and
 * printing, which damage modules that a screen code never meets.
 */
constexpr qrcodegen::QrCode::Ecc kImageEcc = qrcodegen::QrCode::Ecc::MEDIUM;

} // namespace

int QrPhoneDisplayDp(const float page_width_dp, const float page_height_dp) {
  const int by_width = std::clamp(static_cast<int>(page_width_dp) - 2 * kQrPhoneSideMarginDp, 1, kQrPhoneMaxDp);
  const int by_height = std::max(kQrPhoneMinDp, static_cast<int>(page_height_dp) - kQrPhoneReservedHeightDp);
  return std::min(by_width, by_height);
}

Roe<std::string> QrSvgContent(const std::string& text, QrShape* shape) {
  try {
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), kScreenEcc);
    const int size = code.getSize();
    if (shape != nullptr) {
      *shape = QrShape{code.getVersion(), size, static_cast<int>(text.size())};
    }
    const int span = size + 2 * kQuietZone;
    std::ostringstream svg;
    svg << "<rect width=\"" << kQrSvgViewBox << "\" height=\"" << kQrSvgViewBox << "\" fill=\"#ffffff\"/>";
    svg << "<g transform=\"scale(" << static_cast<double>(kQrSvgViewBox) / span << ")\">";
    svg << "<path fill=\"#000000\" shape-rendering=\"crispEdges\" d=\"";
    for (int y = 0; y < size; ++y) {
      for (int x = 0; x < size; ++x) {
        if (code.getModule(x, y)) {
          svg << 'M' << x + kQuietZone << ' ' << y + kQuietZone << "h1v1h-1z";
        }
      }
    }
    svg << "\"/></g>";
    return svg.str();
  } catch (const std::exception& e) {
    return Error(std::string("QR code: ") + e.what()); // text too long for any version
  }
}

Roe<std::vector<uint8_t>> QrPngBytes(const std::string& text, const int target_px) {
  try {
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), kImageEcc);
    const int span = code.getSize() + 2 * kQuietZone;
    const int scale = std::max(1, target_px / span);
    const int side = span * scale;
    SDL_Surface* surface = SDL_CreateSurface(side, side, SDL_PIXELFORMAT_RGB24);
    if (!surface) {
      return Error("QR code: no picture memory");
    }
    auto* pixels = static_cast<uint8_t*>(surface->pixels);
    for (int y = 0; y < side; ++y) {
      uint8_t* row = pixels + static_cast<size_t>(y) * static_cast<size_t>(surface->pitch);
      for (int x = 0; x < side; ++x) {
        const bool dark = code.getModule(x / scale - kQuietZone, y / scale - kQuietZone); // false outside the code
        std::fill_n(row + static_cast<size_t>(x) * 3, 3, static_cast<uint8_t>(dark ? 0x00 : 0xff));
      }
    }
    Roe<std::vector<uint8_t>> png = EncodePngBytes(surface);
    SDL_DestroySurface(surface);
    return png;
  } catch (const std::exception& e) {
    return Error(std::string("QR code: ") + e.what());
  }
}

} // namespace pbr
