#pragma once

#include "feature/presenters/AvatarCropFrame.h"

#include <ui/dom/Element.h>
#include <ui/paint/CallbackTexture.h>
#include <ui/paint/Geometry.h>

#include <array>
#include <cstdint>
#include <vector>

namespace pbr {

/**
 * The avatar crop page's picture: draws the decoded photo as one rotated, scaled textured quad (the corners come from
 * AvatarCropFrame::Quad), the dim outside the round viewport and a 1dp white line round it. Pixels and GPU resources
 * die with the page. The page's controller feeds it; it keeps no state of its own about gestures.
 */
class ElementAvatarCrop : public ui::Element {
public:
  UI_RTTI_DefineWithParent(ElementAvatarCrop, ui::Element) explicit ElementAvatarCrop(const ui::String& tag);

  /** The decoded, upright picture (RGBA, `width` x `height`). The texture is made at the next render. */
  void SetImage(int width, int height, std::vector<uint8_t> rgba);
  /** The picture's corners on screen (dp from the circle's centre) and the circle's diameter (dp). */
  void SetView(const std::array<AvatarCropFrame::Point, 4>& quad_dp, double diameter_dp);

protected:
  void OnRender() override;
  void OnDpRatioChange() override;
  void Release() override;

private:
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> pending_;
  ui::CallbackTexture texture_;
  std::array<AvatarCropFrame::Point, 4> quad_{};
  double diameter_dp_ = 0.0;
  bool has_view_ = false;
  ui::Geometry image_;
  ui::Geometry dim_;  // outside the circle: rebuilt only when the box, diameter, ratio or render manager changes
  ui::Geometry line_; // the white line
  ui::Vector2f dim_box_{0, 0};
  double dim_diameter_ = 0.0;
  float dim_ratio_ = 0.0f;
  ui::RenderManager* dim_manager_ = nullptr;
};

void RegisterAvatarCropElement();

} // namespace pbr
