#pragma once

#include "gui/scan/ScanPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/scan/QrScanner.h"
#include "foundation/runtime/AppLifecycle.h"

#include <functional>

#include <ui/base/ObserverPtr.h>
#include <ui/base/Types.h>
#include <ui/dom/Element.h>

namespace ui {
class Context;
}

namespace pbr {

/** The locale key of the hint for a failed scan, chosen from the scanner's error kind. */
const char* ScanErrorTextKey(QrScanner::ErrorKind kind);

/** Presents a scanner and returns one QR text after closing. Callers own the next action. */
class QrScanController {
public:
  explicit QrScanController(MediaDeviceArbiter& devices);
  ~QrScanController();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  /** The phone scanner's "My QR code" button: the scan closes without a result, then this shows my code. */
  void BindShowMyQr(std::function<void()> show_my_qr);
  bool RegisterModel(ui::Context* context);
  void Open(QrScanResultCallback on_result);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  void Tick();
  /** After Context::Update: keep capture preview responsive during power-save. */
  void NotifyFrameEnd();
  /** Parent calls before UI, device arbiter and SDL teardown. */
  void Shutdown();

private:
  void BeginCapture();
  void OnDismissed();
  void Dirty();
  void FindPreview();

  QrScanner scanner_;
  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  QrScanResultCallback on_result_;
  std::function<void()> show_my_qr_;
  bool can_show_my_qr_ = false;
  ui::Context* context_ = nullptr;
  ui::ObserverPtr<ui::Element> preview_;
  int layer_id_ = -1;
  ui::String status_;
  bool failed_ = false;
  AppLifecycle::ListenerId background_listener_ = 0;
};

} // namespace pbr
