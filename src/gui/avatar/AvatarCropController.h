#pragma once

#include "feature/presenters/AvatarCropFrame.h"
#include "foundation/platform/PickedFileLease.h"
#include "foundation/platform/ProfileIconImagePrep.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/avatar/AvatarCropTouch.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/ObserverPtr.h>
#include <ui/dom/Element.h>
#include <ui/dom/EventListener.h>

#include <functional>
#include <memory>
#include <optional>

namespace ui {
class Context;
}

namespace pbr {

/** The crop page's viewport circle: min(width - 48, height - 48, 320) dp, at least 1. */
double AvatarCropViewportDiameter(double element_w_dp, double element_h_dp);

/** What a mouse wheel notch does: zoom factor for `wheel_delta_y` (positive = down = out), clamped per event. */
double AvatarCropWheelFactor(double wheel_delta_y);

/**
 * The page's state, as a pure decision for "Done" and the buttons: a photo must be on screen and nothing in flight.
 * Done while locked (a double tap) or while loading does nothing.
 */
struct AvatarCropButtons {
  static bool CanDone(bool loading, bool locked, bool has_frame) { return !loading && !locked && has_frame; }
  static bool CanRotate(bool loading, bool locked, bool has_frame) { return CanDone(loading, locked, has_frame); }
  static bool CanCancel(bool locked) { return !locked; }
  /** The page may release the picked file itself only while no "Done" is in flight; after that the PIN step and the
   *  upload hold the file, and the lease releases it when the last of them lets go. */
  static bool PageMayReleaseFile(bool locked) { return !locked; }
};

/** "Done" hands this to the caller; the caller answers `reply.finished(ok)` (true closes the page, false unlocks it)
 *  and tells `reply.uploading(running)` while the upload itself runs (the page shows its "Uploading…" pill then). */
using AvatarCropCommit = std::function<void(ProfileIconCrop crop, AvatarCropReply reply)>;

/**
 * The avatar crop page (PLAN-avatar-edit items 9 and 11): a full-screen page with the picture under a fixed round
 * viewport. Fingers arrive through SdlAppEvents (AvatarCropTouch); the mouse (drag, wheel) through DOM events on the
 * crop element. It holds the picked file's lease from Open until it closes. UI thread.
 */
class AvatarCropController : public ui::EventListener {
public:
  AvatarCropController();
  ~AvatarCropController() override;

  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  bool RegisterModel(ui::Context* context);

  /** Opens the page over `file` (decoded off the UI thread). `commit` is called by "Done". */
  void Open(std::shared_ptr<PickedFileLease> file, AvatarCropCommit commit);
  /** Closes the page and releases the file. */
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  /** Once per frame, before Context::Update: layout changes, summed finger movement, the view for the element. */
  void Tick();
  /** After Context::Update: keep the frame loop turning while the page is open. */
  void NotifyFrameEnd();
  void Shutdown();

  /** SdlAppEvents' finger handler while the page is open; true = taken. */
  bool HandleSdlEvent(ui::Context* context, const SDL_Event& event);

  void ProcessEvent(ui::Event& event) override;

private:
  void OnDismissed();
  void ResetState();
  void Dirty();
  void SyncPaused();
  void OnDecoded(unsigned generation, Roe<ProfilePhotoPreview> preview);
  void DeliverPreview();
  ui::Element* FindSurface();
  void PushView();
  void Done();
  void Rotate();
  void EndMouseDrag();
  CropTouchSpace TouchSpace(ui::Context* context);

  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  unsigned generation_ = 0;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);

  std::shared_ptr<PickedFileLease> file_;
  AvatarCropCommit commit_;
  std::optional<AvatarCropFrame> frame_;
  /** The decoded picture, until the crop element exists and takes it. */
  std::optional<ProfilePhotoPreview> pending_preview_;
  AvatarCropTouch touch_;
  ui::ObserverPtr<ui::Element> surface_;
  ui::ObserverPtr<ui::Element> drag_doc_;
  bool listening_ = false;
  bool dragging_ = false;
  float drag_x_ = 0.0f;
  float drag_y_ = 0.0f;
  bool view_dirty_ = false;
  bool loading_ = false;
  bool locked_ = false;
  bool uploading_ = false;
};

} // namespace pbr
