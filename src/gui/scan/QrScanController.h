#pragma once

#include "gui/scan/ScanPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellNavigationPorts.h"
#include "lib/media/CameraPermission.h"
#include "lib/scan/QrScanner.h"
#include "foundation/runtime/AppLifecycle.h"

#include <functional>
#include <memory>

#include <ui/base/ObserverPtr.h>
#include <ui/base/Types.h>
#include <ui/dom/Element.h>

namespace ui {
class Context;
}

namespace pbr {

/** The locale key of the hint for a failed scan, chosen from the scanner's error kind. */
const char* ScanErrorTextKey(QrScanner::ErrorKind kind);

/** The camera permission the scanner asks before its page opens. Empty members take the platform's (tests set them). */
struct QrScanCameraPorts {
  std::function<CameraPermission()> state;
  /** Shows the OS prompt; `done(granted)` runs once, on any thread. */
  std::function<void(std::function<void(bool granted)>)> request;
  std::function<void(std::function<void()>)> post_ui;
};

/** Presents a scanner and returns one QR text after closing. Callers own the next action. */
class QrScanController {
public:
  explicit QrScanController(MediaDeviceArbiter& devices, QrScanCameraPorts camera = {});
  ~QrScanController();

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  /** The phone scanner's "My QR code" button: the scan closes without a result, then this shows my code. */
  void BindShowMyQr(std::function<void()> show_my_qr);
  bool RegisterModel(ui::Context* context);
  /**
   * Every scan entry comes here. With the camera permission not asked yet, the OS prompt comes first and no page is
   * pushed under it (the prompt takes the app out of the foreground, which closes an open scanner); "Allow" then
   * opens the page by itself once the app is in the foreground again, "Don't allow" (or a refusal made earlier) shows
   * the "allow the camera in Settings" pill instead.
   */
  void Open(QrScanResultCallback on_result) { Open(std::move(on_result), {}); }
  /**
   * The same, and `on_closed` once the request ends without a text: cancelled, dismissed, the camera refused, the app
   * sent to the background, or a newer Open. Never after `on_result`, never during Shutdown.
   */
  void Open(QrScanResultCallback on_result, std::function<void()> on_closed);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  void Tick();
  /** After Context::Update: keep capture preview responsive during power-save. */
  void NotifyFrameEnd();
  /** Parent calls before UI, device arbiter and SDL teardown. */
  void Shutdown();

private:
  void Present(QrScanResultCallback on_result);
  /** Stops the camera and closes the page; the request's callbacks are not told (Close and OnDismissed do that). */
  void CloseSurface();
  /** The request ended without a text: its `on_closed`, once. */
  void EndWithoutResult();
  void OnCameraAnswer(bool granted);
  void BeginCapture();
  void OnDismissed();
  void Dirty();
  void FindPreview();

  QrScanner scanner_;
  QrScanCameraPorts camera_;
  /** The tap waiting for the OS prompt's answer; the newest tap's callback wins. */
  QrScanResultCallback awaiting_permission_;
  bool asking_ = false;
  /** The answer came while the app was not in the foreground: run on the next foreground transition. */
  std::function<void()> on_foreground_;
  /** Turns false in the destructor: a posted answer finds the controller gone. */
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  QrScanResultCallback on_result_;
  std::function<void()> on_closed_;
  std::function<void()> show_my_qr_;
  bool can_show_my_qr_ = false;
  ui::Context* context_ = nullptr;
  ui::ObserverPtr<ui::Element> preview_;
  int layer_id_ = -1;
  ui::String status_;
  bool failed_ = false;
  AppLifecycle::ListenerId background_listener_ = 0;
  AppLifecycle::ListenerId foreground_listener_ = 0;
};

} // namespace pbr
