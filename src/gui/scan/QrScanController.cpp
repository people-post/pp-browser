#include "gui/scan/QrScanController.h"

#include "gui/scan/ElementScanPreview.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "lib/media/CameraCaptureOrientation.h"

#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>

namespace pbr {

QrScanController::QrScanController(MediaDeviceArbiter& devices) : scanner_(devices) {
  background_listener_ = AppLifecycle::AddBackgroundListener([this]() { Close(); });
}

QrScanController::~QrScanController() {
  AppLifecycle::RemoveListener(background_listener_);
  scanner_.Shutdown();
}

void QrScanController::BindShellNavigation(ShellNavigationPorts ports) { navigation_ = std::move(ports); }
void QrScanController::BindFlowCoordinator(FlowCoordinatorPorts ports) { flow_ = std::move(ports); }

bool QrScanController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "qr_scan", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("status", &status_);
    ctor.Bind("failed", &failed_);
    ctor.BindEventCallback("cancel_scan", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { Close(); });
    ctor.BindEventCallback("retry_scan", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      if (IsOpen()) {
        BeginCapture();
      }
    });
  });
}

void QrScanController::Open(QrScanResultCallback on_result) {
  Close();
  if (!navigation_.push_layer || !on_result || !AppLifecycle::IsForeground()) {
    return;
  }
  PaneSpec spec;
  spec.key = "qr_scan";
  spec.rml_path = "qr_scan";
  spec.role = PaneRole::Transient;
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
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

void QrScanController::OnDismissed() {
  layer_id_ = -1;
  scanner_.Stop();
  on_result_ = nullptr;
}

void QrScanController::Close() {
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
  if (auto* window = Backend::GetWindow(); window &&
      (SDL_GetWindowFlags(window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0) {
    Close();
    return;
  }
#endif
  auto update = scanner_.Poll();
  if (!update.error.empty()) {
    status_ = Tr("scan.camera_error");
    failed_ = true;
    Dirty();
  }
  if (update.text) {
    auto done = std::move(on_result_);
    Close();
    if (done) {
      done(std::move(*update.text));
    }
    return;
  }
  if (update.preview && context_) {
    status_ = Tr("scan.hint");
    for (int i = 0; i < context_->GetNumDocuments(); ++i) {
      auto* document = context_->GetDocument(i);
      if (!document) {
        continue;
      }
      auto* element = document->GetElementById("qr-scan-preview");
      if (auto* preview = dynamic_cast<ElementScanPreview*>(element)) {
        preview->SetFrame(update.preview->width, update.preview->height, std::move(update.preview->rgba));
        break;
      }
    }
    Dirty();
  }
}

void QrScanController::NotifyFrameEnd() {
  if (IsOpen() && context_) {
    context_->RequestNextUpdate(.05);
  }
}

void QrScanController::Dirty() {
  if (context_) {
    DataModelHost::Instance().Dirty("qr_scan", "status");
    DataModelHost::Instance().Dirty("qr_scan", "failed");
  }
}

void QrScanController::Shutdown() {
  Close();
  scanner_.Shutdown();
}

} // namespace pbr
