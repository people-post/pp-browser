#include "gui/avatar/AvatarCropController.h"

#include "common/Logger.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/error/AppError.h"
#include "foundation/platform/ui/SdlAppEvents.h"
#include "gui/UserFeedback.h"
#include "gui/avatar/ElementAvatarCrop.h"
#include "gui/shell/DataModelHost.h"

#include <ui/data/DataModelHandle.h>
#include <ui/dom/Context.h>
#include <ui/dom/ElementDocument.h>

#include <algorithm>
#include <cmath>

namespace pbr {

namespace {
logging::Logger& CropLog() {
  static logging::Logger log = logging::getLogger("Gui.AvatarCrop");
  return log;
}
constexpr double kMargin = 48.0;
constexpr double kMaxDiameter = 320.0;
// One wheel unit scrolls 80 dp in the UI library (Context.cpp UNIT_SCROLL_LENGTH); the zoom reads it as that many px.
constexpr double kWheelUnitPx = 80.0;
} // namespace

double AvatarCropViewportDiameter(const double element_w_dp, const double element_h_dp) {
  return std::max(1.0, std::min({element_w_dp - kMargin, element_h_dp - kMargin, kMaxDiameter}));
}

double AvatarCropWheelFactor(const double wheel_delta_y) {
  if (!std::isfinite(wheel_delta_y)) {
    return 1.0;
  }
  return std::clamp(std::pow(1.0015, -wheel_delta_y * kWheelUnitPx), 0.9, 1.1);
}

AvatarCropController::AvatarCropController() = default;

AvatarCropController::~AvatarCropController() {
  *alive_ = false;
  SdlAppEvents::SetFingerHandler({});
}

void AvatarCropController::BindShellNavigation(ShellNavigationPorts ports) {
  navigation_ = std::move(ports);
}
void AvatarCropController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_ = std::move(ports);
}

bool AvatarCropController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "avatar_crop", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("crop_loading", &loading_);
    ctor.Bind("crop_locked", &locked_);
    ctor.Bind("crop_uploading", &uploading_);
    ctor.BindEventCallback("crop_cancel", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      if (AvatarCropButtons::CanCancel(locked_)) {
        Close();
      }
    });
    ctor.BindEventCallback("crop_rotate",
                           [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { Rotate(); });
    ctor.BindEventCallback("crop_done", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { Done(); });
  });
}

void AvatarCropController::Dirty() {
  if (context_) {
    DataModelHost::Instance().Dirty("avatar_crop", "crop_loading");
    DataModelHost::Instance().Dirty("avatar_crop", "crop_locked");
    DataModelHost::Instance().Dirty("avatar_crop", "crop_uploading");
  }
}

void AvatarCropController::Open(std::shared_ptr<PickedFileLease> file, AvatarCropCommit commit) {
  if (IsOpen()) {
    // One crop page at a time: a second pick that arrives late must not replace the page the user is working on.
    CropLog().warning << "Open ignored: a crop page is already open";
    if (file) {
      file->Release();
    }
    return;
  }
  if (!file || !commit || !navigation_.push_layer) {
    CropLog().warning << "Open ignored: nothing to crop or push_layer is not bound";
    if (file) {
      file->Release();
    }
    return;
  }
  PaneSpec spec;
  spec.key = "avatar_crop";
  spec.rml_path = "avatar_crop";
  spec.role = PaneRole::Transient;
  spec.full_screen = true;
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
    CropLog().warning << "Open ignored: push_layer returned no layer";
    file->Release();
    return;
  }
  file_ = std::move(file);
  commit_ = std::move(commit);
  loading_ = true;
  locked_ = false;
  const unsigned generation = ++generation_;
  if (flow_.begin_modal) {
    // Back while "Done" is in flight is swallowed; otherwise it cancels the page (OnDismissed).
    flow_.begin_modal(layer_id_, [this]() { return locked_; }, [this]() { OnDismissed(); });
  }
  SdlAppEvents::SetFingerHandler(
      [this](ui::Context* context, const SDL_Event& event) { return HandleSdlEvent(context, event); });
  SyncPaused();
  Dirty();
  const std::string path = file_->path();
  const std::weak_ptr<bool> alive = alive_;
  AppRuntime::PostWorkerNormal([this, alive, generation, path]() {
    Roe<ProfilePhotoPreview> preview = DecodeProfilePhotoPreview(path);
    AppRuntime::PostUI([this, alive, generation, preview = std::move(preview)]() mutable {
      if (alive.expired()) {
        return;
      }
      OnDecoded(generation, std::move(preview));
    });
  });
}

void AvatarCropController::OnDecoded(const unsigned generation, Roe<ProfilePhotoPreview> preview) {
  if (generation != generation_ || !IsOpen()) {
    return;
  }
  if (!preview) {
    loading_ = false;
    CropLog().warning << "Decode failed: " << AppError::Log(preview.error());
    UserFeedback::FailFrom(preview.error());
    Close();
    return;
  }
  frame_.emplace(preview->full_width, preview->full_height, 1.0);
  pending_preview_ = std::move(*preview);
  // The page stays "loading" until the element has the picture: Tick delivers it as soon as the element exists.
  DeliverPreview();
}

void AvatarCropController::DeliverPreview() {
  if (!pending_preview_ || !frame_) {
    return;
  }
  auto* crop = dynamic_cast<ElementAvatarCrop*>(FindSurface());
  if (!crop) {
    return;
  }
  const ui::Vector2f box = crop->GetBox().GetSize(ui::BoxArea::Content);
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.0f;
  frame_->SetViewportDiameter(AvatarCropViewportDiameter(box.x / ratio, box.y / ratio));
  crop->SetImage(pending_preview_->width, pending_preview_->height, std::move(pending_preview_->rgba));
  pending_preview_.reset();
  loading_ = false;
  view_dirty_ = true;
  SyncPaused();
  Dirty();
}

ui::Element* AvatarCropController::FindSurface() {
  if (surface_ && dynamic_cast<ElementAvatarCrop*>(surface_.get())) {
    return surface_.get();
  }
  if (!context_) {
    return nullptr;
  }
  for (int i = 0; i < context_->GetNumDocuments(); ++i) {
    auto* document = context_->GetDocument(i);
    auto* element = document ? document->GetElementById("avatar-crop-surface") : nullptr;
    if (dynamic_cast<ElementAvatarCrop*>(element)) {
      surface_ = element->GetObserverPtr();
      element->AddEventListener(ui::EventId::Mousedown, this);
      element->AddEventListener(ui::EventId::Mousescroll, this);
      listening_ = true;
      return element;
    }
  }
  return nullptr;
}

void AvatarCropController::SyncPaused() {
  // Loading, or "Done" in flight (a PIN pad may be over the page): every finger passes through untouched.
  touch_.SetPaused(loading_ || locked_ || !IsOpen());
}

CropTouchSpace AvatarCropController::TouchSpace(ui::Context* context) {
  CropTouchSpace space;
  const ui::Vector2i dims = context->GetDimensions();
  space.window_w_px = static_cast<float>(dims.x);
  space.window_h_px = static_cast<float>(dims.y);
  space.px_per_dp = context->GetDensityIndependentPixelRatio();
  if (ui::Element* surface = FindSurface()) {
    const float ratio = space.px_per_dp > 0.0f ? space.px_per_dp : 1.0f;
    const ui::Vector2f at = surface->GetAbsoluteOffset(ui::BoxArea::Content);
    const ui::Vector2f size = surface->GetBox().GetSize(ui::BoxArea::Content);
    space.box_x = at.x / ratio;
    space.box_y = at.y / ratio;
    space.box_w = size.x / ratio;
    space.box_h = size.y / ratio;
  }
  return space;
}

bool AvatarCropController::HandleSdlEvent(ui::Context* context, const SDL_Event& event) {
  if (!IsOpen() || !context) {
    return false;
  }
  return touch_.OnSdlEvent(event, TouchSpace(context));
}

void AvatarCropController::Tick() {
  if (!IsOpen()) {
    return;
  }
  ui::Element* surface = FindSurface();
  if (!surface || !frame_ || !context_) {
    return;
  }
  DeliverPreview();
  const ui::Vector2f box = surface->GetBox().GetSize(ui::BoxArea::Content);
  const float ratio =
      context_->GetDensityIndependentPixelRatio() > 0.0f ? context_->GetDensityIndependentPixelRatio() : 1.0f;
  const double diameter = AvatarCropViewportDiameter(box.x / ratio, box.y / ratio);
  if (diameter != frame_->viewport_diameter()) {
    frame_->SetViewportDiameter(diameter);
    view_dirty_ = true;
  }
  if (touch_.Apply(*frame_)) {
    view_dirty_ = true;
  }
  if (view_dirty_) {
    PushView();
  }
}

void AvatarCropController::PushView() {
  view_dirty_ = false;
  auto* crop = dynamic_cast<ElementAvatarCrop*>(surface_.get());
  if (crop && frame_) {
    crop->SetView(frame_->Quad(), frame_->viewport_diameter());
  }
}

void AvatarCropController::NotifyFrameEnd() {
  if (IsOpen() && context_ && (touch_.Tracking() || dragging_ || loading_)) {
    context_->RequestNextUpdate(1.0 / 60.0);
  }
}

void AvatarCropController::Rotate() {
  if (!AvatarCropButtons::CanRotate(loading_, locked_, frame_.has_value())) {
    return;
  }
  frame_->RotateQuarter();
  PushView();
}

void AvatarCropController::Done() {
  if (!AvatarCropButtons::CanDone(loading_, locked_, frame_.has_value()) || !commit_) {
    return;
  }
  // Freeze everything: buttons off, fingers and mouse stop, the parameters are read once, now. The movement already
  // summed this frame is applied first (pausing the touch drops it): it is part of what the user sees.
  Tick();
  locked_ = true;
  EndMouseDrag();
  SyncPaused();
  Dirty();
  ProfileIconCrop crop;
  const int side = std::max(1, static_cast<int>(std::lround(std::min(1024.0, frame_->CircleDiameterImagePx()))));
  crop.side = side;
  crop.affine = frame_->OutputAffine(side);
  const unsigned generation = generation_;
  const std::weak_ptr<bool> alive = alive_;
  AvatarCropReply reply;
  reply.uploading = [this, alive, generation](const bool running) {
    if (alive.expired() || generation != generation_ || !IsOpen()) {
      return;
    }
    uploading_ = running;
    Dirty();
  };
  reply.finished = [this, alive, generation](const bool ok) {
    if (alive.expired() || generation != generation_ || !IsOpen()) {
      return;
    }
    uploading_ = false;
    if (ok) {
      Close();
      return;
    }
    // A refused PIN or a failed upload: the page is as the user left it; they can try again.
    locked_ = false;
    SyncPaused();
    Dirty();
  };
  commit_(crop, std::move(reply));
}

void AvatarCropController::ProcessEvent(ui::Event& event) {
  if (locked_ || loading_ || !frame_ || !context_) {
    return;
  }
  const float ratio =
      context_->GetDensityIndependentPixelRatio() > 0.0f ? context_->GetDensityIndependentPixelRatio() : 1.0f;
  const float x = event.GetParameter<float>("mouse_x", 0.0f);
  const float y = event.GetParameter<float>("mouse_y", 0.0f);
  switch (event.GetId()) {
    case ui::EventId::Mousedown: {
      if (event.GetParameter<int>("button", 0) != 0 || dragging_) {
        return;
      }
      ui::Element* target = event.GetTargetElement();
      ui::ElementDocument* doc = target ? target->GetOwnerDocument() : nullptr;
      if (!doc) {
        return;
      }
      dragging_ = true;
      drag_x_ = x;
      drag_y_ = y;
      drag_doc_ = doc->GetObserverPtr();
      doc->AddEventListener(ui::EventId::Mousemove, this, true);
      doc->AddEventListener(ui::EventId::Mouseup, this, true);
      break;
    }
    case ui::EventId::Mousemove:
      if (dragging_) {
        frame_->Pan((x - drag_x_) / ratio, (y - drag_y_) / ratio);
        drag_x_ = x;
        drag_y_ = y;
        PushView();
      }
      break;
    case ui::EventId::Mouseup:
      EndMouseDrag();
      break;
    case ui::EventId::Mousescroll: {
      ui::Element* surface = surface_.get();
      if (!surface) {
        return;
      }
      const ui::Vector2f at = surface->GetAbsoluteOffset(ui::BoxArea::Content);
      const ui::Vector2f size = surface->GetBox().GetSize(ui::BoxArea::Content);
      const AvatarCropFrame::Point focus{(x - (at.x + size.x * 0.5f)) / ratio, (y - (at.y + size.y * 0.5f)) / ratio};
      frame_->ZoomAt(focus, AvatarCropWheelFactor(event.GetParameter<float>("wheel_delta_y", 0.0f)));
      PushView();
      event.StopPropagation(); // the page must not scroll under the wheel
      break;
    }
    default:
      break;
  }
}

void AvatarCropController::EndMouseDrag() {
  dragging_ = false;
  if (drag_doc_) {
    drag_doc_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    drag_doc_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  drag_doc_ = nullptr;
}

void AvatarCropController::ResetState() {
  const bool done_in_flight = locked_;
  EndMouseDrag();
  if (surface_ && listening_) {
    surface_->RemoveEventListener(ui::EventId::Mousedown, this);
    surface_->RemoveEventListener(ui::EventId::Mousescroll, this);
  }
  listening_ = false;
  surface_ = nullptr;
  SdlAppEvents::SetFingerHandler({});
  touch_.Clear();
  frame_.reset();
  commit_ = nullptr;
  loading_ = false;
  locked_ = false;
  uploading_ = false;
  view_dirty_ = false;
  ++generation_; // a decode still running, or a "Done" still answering, is ignored from here on
  layer_id_ = -1;
  pending_preview_.reset();
  if (file_) {
    // Done in flight: the PIN step or the upload worker may be reading the file, and they hold their own references:
    // the page only lets go of its own and the lease releases the file when the last holder is gone. Otherwise the
    // page was the file's holder and it goes now, whichever way the page ended.
    if (AvatarCropButtons::PageMayReleaseFile(done_in_flight)) {
      file_->Release();
    }
    file_.reset();
  }
  Dirty();
}

// The shell closed the page itself (back key, swipe, a layer sweep).
void AvatarCropController::OnDismissed() {
  ResetState();
}

void AvatarCropController::Close() {
  const int layer = layer_id_;
  ResetState();
  if (layer >= 0) {
    if (flow_.end_modal) {
      flow_.end_modal();
    }
    if (navigation_.close_layer) {
      navigation_.close_layer(layer);
    }
  }
}

void AvatarCropController::Shutdown() {
  Close();
}

} // namespace pbr
