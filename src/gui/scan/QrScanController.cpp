#include "gui/scan/QrScanController.h"

#include "gui/scan/ElementScanPreview.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "lib/media/CameraCaptureOrientation.h"

#include "common/Logger.h"
#include "common/runtime/AppRuntime.h"

#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>

#include <utility>

namespace pbr {

namespace {
logging::Logger& ScanLog() {
  static logging::Logger log = logging::getLogger("Gui.QrScan");
  return log;
}
} // namespace

QrScanController::QrScanController(MediaDeviceArbiter& devices, QrScanCameraPorts camera)
    : scanner_(devices), camera_(std::move(camera)) {
  if (!camera_.state) {
    camera_.state = []() { return CameraPermissionState(); };
  }
  if (!camera_.request) {
    camera_.request = [](std::function<void(bool)> done) { RequestCameraPermission(std::move(done)); };
  }
  if (!camera_.post_ui) {
    camera_.post_ui = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  }
  // A real trip to the background closes the scanner (the camera is not kept on behind the app). The OS camera
  // prompt does this too, which is why Open asks before it pushes the page.
  background_listener_ = AppLifecycle::AddBackgroundListener([this]() { Close(); });
  foreground_listener_ = AppLifecycle::AddForegroundListener([this]() {
    if (!on_foreground_) {
      return;
    }
    // Out of the lifecycle callback: the page is pushed from the UI loop, like any other tap.
    camera_.post_ui([this, alive = alive_]() {
      if (!*alive || !AppLifecycle::IsForeground()) {
        return;
      }
      if (auto next = std::exchange(on_foreground_, nullptr)) {
        next();
      }
    });
  });
}

QrScanController::~QrScanController() {
  *alive_ = false;
  AppLifecycle::RemoveListener(background_listener_);
  AppLifecycle::RemoveListener(foreground_listener_);
  scanner_.Shutdown();
}

void QrScanController::BindShellNavigation(ShellNavigationPorts ports) {
  navigation_ = std::move(ports);
}
void QrScanController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_ = std::move(ports);
}
void QrScanController::BindShowMyQr(std::function<void()> show_my_qr) {
  show_my_qr_ = std::move(show_my_qr);
  can_show_my_qr_ = static_cast<bool>(show_my_qr_);
}

bool QrScanController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "qr_scan", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("status", &status_);
    ctor.Bind("failed", &failed_);
    ctor.Bind("can_show_my_qr", &can_show_my_qr_);
    ctor.BindEventCallback("cancel_scan", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { Close(); });
    ctor.BindEventCallback("retry_scan", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      if (IsOpen()) {
        BeginCapture();
      }
    });
    // Like WeChat: "My QR code" under the frame leaves the scan (no result) and shows my code instead.
    ctor.BindEventCallback("scan_show_my_qr", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      Close();
      if (show_my_qr_) {
        show_my_qr_();
      }
    });
  });
}

void QrScanController::Open(QrScanResultCallback on_result, std::function<void()> on_closed) {
  ScanLog().info << "Open requested (camera permission " << static_cast<int>(camera_.state()) << ")";
  Close();
  const auto ignored = [&on_closed](const char* why) {
    ScanLog().warning << "Open ignored: " << why;
    if (on_closed) {
      on_closed(); // this request ends here; one still waiting (the permission prompt) keeps its own
    }
  };
  if (!navigation_.push_layer) {
    ignored("push_layer is not bound");
    return;
  }
  if (!on_result) {
    ignored("empty result callback");
    return;
  }
  if (!AppLifecycle::IsForeground()) {
    ignored("app is not in the foreground");
    return;
  }
  // A newer tap takes over: whatever an older tap left waiting (a late permission answer, a stored "when the app is
  // active again") belongs to an abandoned request and must not run after this one.
  awaiting_permission_ = nullptr;
  on_foreground_ = nullptr;
  EndWithoutResult();
  on_closed_ = std::move(on_closed);
  switch (camera_.state()) {
    case CameraPermission::Granted:
      Present(std::move(on_result));
      return;
    case CameraPermission::Denied:
      UserFeedback::Fail(Tr("scan.camera_denied"));
      EndWithoutResult();
      return;
    case CameraPermission::NotDetermined:
      break;
  }
  awaiting_permission_ = std::move(on_result);
  if (asking_) {
    return; // the prompt is already up: its answer serves this tap
  }
  asking_ = true;
  ScanLog().info << "Asking for the camera permission before the scan page";
  camera_.request([post_ui = camera_.post_ui, alive = alive_, this](const bool granted) {
    post_ui([alive, this, granted]() {
      if (*alive) {
        OnCameraAnswer(granted);
      }
    });
  });
}

void QrScanController::OnCameraAnswer(const bool granted) {
  asking_ = false;
  auto on_result = std::exchange(awaiting_permission_, nullptr);
  if (!on_result) {
    return; // shut down meanwhile
  }
  ScanLog().info << "Camera permission answered: " << (granted ? "granted" : "refused");
  std::function<void()> next;
  if (granted) {
    next = [this, on_result = std::move(on_result)]() mutable { Present(std::move(on_result)); };
  } else {
    next = [this]() {
      UserFeedback::Fail(Tr("scan.camera_denied"));
      EndWithoutResult();
    };
  }
  // The answer may land before the app is active again (iOS hands it over while its prompt is going away): the page
  // then opens on the next foreground transition rather than being dropped.
  if (AppLifecycle::IsForeground()) {
    next();
  } else {
    on_foreground_ = std::move(next);
  }
}

void QrScanController::Present(QrScanResultCallback on_result) {
  CloseSurface();
  if (!navigation_.push_layer) {
    EndWithoutResult();
    return;
  }
  PaneSpec spec;
  spec.key = "qr_scan";
  spec.rml_path = "qr_scan";
  spec.role = PaneRole::Transient;
  spec.full_screen = true; // phones: the camera fills the screen, WeChat style (Kenneth 2026-11-04)
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
    ScanLog().warning << "Open ignored: push_layer returned no layer";
    EndWithoutResult();
    return;
  }
  on_result_ = std::move(on_result);
  if (flow_.begin_modal) {
    flow_.begin_modal(layer_id_, []() { return false; }, [this]() { OnDismissed(); });
  }
  BeginCapture();
}

void QrScanController::BeginCapture() {
  status_ = Tr("scan.opening");
  failed_ = false;
  // iOS preview is portrait-locked; scanning need not start the call's gravity sensor.
  const int rotation = context_ && !CameraRotationFollowsDevice() ? CameraDisplayRotationDegrees() : 0;
  scanner_.Start(rotation);
  Dirty();
}

const char* ScanErrorTextKey(const QrScanner::ErrorKind kind) {
  switch (kind) {
    case QrScanner::ErrorKind::PermissionDenied:
      return "scan.camera_denied";
    case QrScanner::ErrorKind::CameraBusy:
      return "scan.camera_busy";
    case QrScanner::ErrorKind::None:
    case QrScanner::ErrorKind::Other:
      break;
  }
  return "scan.camera_error";
}

void QrScanController::OnDismissed() {
  const bool pending = static_cast<bool>(on_result_);
  preview_ = nullptr;
  layer_id_ = -1;
  scanner_.Stop();
  on_result_ = nullptr;
  if (pending) {
    EndWithoutResult();
  }
}

void QrScanController::Close() {
  const bool pending = static_cast<bool>(on_result_);
  CloseSurface();
  if (pending) {
    EndWithoutResult();
  }
}

void QrScanController::EndWithoutResult() {
  if (auto closed = std::exchange(on_closed_, nullptr)) {
    closed();
  }
}

void QrScanController::CloseSurface() {
  preview_ = nullptr;
  scanner_.Stop();
  on_result_ = nullptr;
  const int layer = layer_id_;
  layer_id_ = -1;
  if (layer >= 0) {
    if (flow_.end_modal) {
      flow_.end_modal();
    }
    if (navigation_.close_layer) {
      navigation_.close_layer(layer);
    }
  }
}

void QrScanController::Tick() {
  if (!IsOpen()) {
    return;
  }
#if UI_SDL_VERSION_MAJOR >= 3
  if (auto* window = Backend::GetWindow();
      window && (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
    Close();
    return;
  }
#endif
  auto update = scanner_.Poll();
  if (!update.error.empty()) {
    // Hint from the diagnosed cause, never a blanket "check permissions".
    status_ = Tr(ScanErrorTextKey(update.error_kind));
    failed_ = true;
    Dirty();
  }
  if (update.text) {
    auto done = std::move(on_result_);
    on_result_ = nullptr;
    on_closed_ = nullptr; // a text came: this request is answered
    Close();
    if (done) {
      done(std::move(*update.text));
    }
    return;
  }
  if (update.preview && context_) {
    status_ = Tr("scan.hint");
    if (!preview_) {
      FindPreview();
    }
    auto* preview = dynamic_cast<ElementScanPreview*>(preview_.get());
    if (!preview && preview_) {
      preview_ = nullptr; // the id now names something else: look again
      FindPreview();
      preview = dynamic_cast<ElementScanPreview*>(preview_.get());
    }
    if (preview) {
      preview->SetFrame(update.preview->width, update.preview->height, std::move(update.preview->rgba));
    }
    Dirty();
  }
}

// Looked up once per scan surface; the observer pointer empties itself when the element goes away.
void QrScanController::FindPreview() {
  for (int i = 0; i < context_->GetNumDocuments(); ++i) {
    auto* document = context_->GetDocument(i);
    auto* element = document ? document->GetElementById("qr-scan-preview") : nullptr;
    if (dynamic_cast<ElementScanPreview*>(element)) {
      preview_ = element->GetObserverPtr();
      return;
    }
  }
}

void QrScanController::NotifyFrameEnd() {
  if (IsOpen() && context_) {
    context_->RequestNextUpdate(1.0 / 60.0);
  }
}

void QrScanController::Dirty() {
  if (context_) {
    DataModelHost::Instance().Dirty("qr_scan", "status");
    DataModelHost::Instance().Dirty("qr_scan", "failed");
  }
}

void QrScanController::Shutdown() {
  awaiting_permission_ = nullptr;
  on_foreground_ = nullptr;
  on_closed_ = nullptr; // the callers are going too: nothing is told
  Close();
  scanner_.Shutdown();
}

} // namespace pbr
