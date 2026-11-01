#pragma once

#include "gui/scan/ScanPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/media/QrScanner.h"
#include "foundation/runtime/AppLifecycle.h"

#include <ui/base/Types.h>

namespace ui {
class Context;
}

namespace pbr {

/** Presents a scanner and returns one QR text after closing. Callers own the next action. */
class QrScanController {
public:
  explicit QrScanController(MediaDeviceArbiter& devices);
  ~QrScanController();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
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

  QrScanner scanner_;
  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  QrScanResultCallback on_result_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  ui::String status_;
  bool failed_ = false;
  AppLifecycle::ListenerId background_listener_ = 0;
};

} // namespace pbr
