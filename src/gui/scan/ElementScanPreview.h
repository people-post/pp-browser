#pragma once

#include <ui/base/Types.h>
#include <ui/dom/Element.h>
#include <ui/paint/CallbackTexture.h>
#include <ui/paint/Geometry.h>

#include <cstdint>
#include <vector>

namespace pbr {

/**
 * Where the picture and the guide square go in a preview box. `whole_frame` (a computer's full-window scan layer): the
 * whole frame, aspect-fit and centred, black bars on the spare sides, and a guide square of 70% of the picture's
 * shorter side in the middle of the picture. Otherwise (the phone's layer and the framed panel): the picture covers
 * the box, and the guide is the 68% square of the box.
 */
struct ScanPreviewLayout {
  ui::Vector2f image_offset;
  ui::Vector2f image_size;
  /** The guide's side, in the same units; it is centred on the picture (whole_frame) or the box. */
  float guide_side = 0.f;
};
inline constexpr float kScanGuideShare = 0.7f;
ScanPreviewLayout LayoutScanPreview(ui::Vector2f box, int frame_width, int frame_height, bool whole_frame);

/** UI-owned camera preview. Pixels and GPU resources die with the scan surface. */
class ElementScanPreview : public ui::Element {
public:
  UI_RTTI_DefineWithParent(ElementScanPreview, ui::Element) explicit ElementScanPreview(const ui::String& tag);
  void SetFrame(int width, int height, std::vector<uint8_t> rgba);
  /** Draw the picture as a mirror (a computer's front camera). Only the drawing: the pixels given to SetFrame are kept
   *  as they are, and the scanner's decoder never sees this flag. Honoured only in a computer's scan layer. */
  void SetMirrored(bool mirrored) { mirrored_ = mirrored; }

protected:
  void OnRender() override;
  void OnDpRatioChange() override;
  void Release() override;

private:
  int width_ = 0;
  int height_ = 0;
  bool mirrored_ = false;
  std::vector<uint8_t> pending_;
  ui::CallbackTexture texture_;
  ui::Geometry geometry_;
  ui::Geometry frame_;     // dimming + corner marks, rebuilt only when the box or render manager changes
  ui::Geometry scan_line_; // rebuilt every frame
  ui::Vector2f frame_box_{0, 0};
  ui::RenderManager* frame_manager_ = nullptr;
};

void RegisterScanPreviewElement();

} // namespace pbr
