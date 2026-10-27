#pragma once

#include <functional>
#include <string>

namespace pbr {

using QrScanResultCallback = std::function<void(std::string)>;

/** App-bound scanning entry. The caller interprets the text; capture never executes its contents. */
struct ScanPorts {
  std::function<void(QrScanResultCallback)> scan_qr;
};

} // namespace pbr
