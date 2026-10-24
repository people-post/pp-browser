#include "lib/ui/QrSvg.h"

#include <qrcodegen.hpp>

#include <exception>
#include <sstream>

namespace pbr {

namespace {

/** The blank modules a scanner needs around the code. */
constexpr int kQuietZone = 4;

} // namespace

Roe<std::string> QrSvgContent(const std::string& text) {
  try {
    const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    const int size = code.getSize();
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
    return Error(std::string("QR code: ") + e.what());  // text too long for any version
  }
}

} // namespace pbr
