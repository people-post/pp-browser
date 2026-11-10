#include "gui/avatar/ElementAvatarCrop.h"

#include "gui/avatar/AvatarCropMesh.h"

#include <ui/dom/Context.h>
#include <ui/dom/ElementInstancer.h>
#include <ui/dom/ElementUtilities.h>
#include <ui/dom/Factory.h>
#include <ui/paint/RenderManager.h>

namespace pbr {
namespace {
constexpr int kRingSegments = 64;
} // namespace

ElementAvatarCrop::ElementAvatarCrop(const ui::String& tag) : ui::Element(tag) {}

void ElementAvatarCrop::SetImage(const int width, const int height, std::vector<uint8_t> rgba) {
  if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
    return;
  }
  width_ = width;
  height_ = height;
  pending_ = std::move(rgba);
}

void ElementAvatarCrop::SetView(const std::array<AvatarCropFrame::Point, 4>& quad_dp, const double diameter_dp) {
  quad_ = quad_dp;
  diameter_dp_ = diameter_dp;
  has_view_ = true;
}

void ElementAvatarCrop::OnRender() {
  auto* manager = GetRenderManager();
  ui::Context* context = GetContext();
  if (!manager || !context || width_ <= 0 || height_ <= 0 || !has_view_) {
    return;
  }
  if (!pending_.empty()) {
    texture_ = manager->MakeCallbackTexture([pixels = std::move(pending_), dimensions = ui::Vector2i(width_, height_)](
                                                const ui::CallbackTextureInterface& target) {
      return target.GenerateTexture({pixels.data(), pixels.size()}, dimensions);
    });
    pending_.clear();
  }
  const ui::Vector2f box = GetBox().GetSize(ui::BoxArea::Content);
  if (box.x <= 0 || box.y <= 0) {
    return;
  }
  // A custom element's geometry is not clipped by an `overflow: hidden` ancestor unless that ancestor's own content
  // overflows (ElementUtilities::GetClippingRegion); the turned picture always reaches past the box. Clip to this
  // element's own box (and every clipping ancestor) before drawing.
  ui::ElementUtilities::SetClippingRegion(this, true);
  const float ratio = context->GetDensityIndependentPixelRatio();
  const ui::Vector2f centre(box.x * .5f, box.y * .5f);
  const ui::Vector2f offset = GetAbsoluteOffset(ui::BoxArea::Content);

  image_ = manager->MakeGeometry(AvatarCropImageMesh(quad_, ratio, centre));
  image_.Render(offset, texture_);

  if (manager != dim_manager_ || box != dim_box_ || diameter_dp_ != dim_diameter_ || ratio != dim_ratio_) {
    dim_manager_ = manager;
    dim_box_ = box;
    dim_diameter_ = diameter_dp_;
    dim_ratio_ = ratio;
    const float radius = static_cast<float>(diameter_dp_) * ratio * .5f;
    dim_ = manager->MakeGeometry(
        AvatarCropDimMesh(box, centre, radius, kRingSegments, ui::ColourbPremultiplied(0, 0, 0, 150)));
    line_ = manager->MakeGeometry(
        AvatarCropRingMesh(centre, radius, ratio, kRingSegments, ui::ColourbPremultiplied(255, 255, 255, 255)));
  }
  dim_.Render(offset);
  line_.Render(offset);
}

// Pixel sizes derive from the dp ratio, so the cached geometry is stale after it changes.
void ElementAvatarCrop::OnDpRatioChange() {
  ui::Element::OnDpRatioChange();
  dim_manager_ = nullptr;
}

// Drop the GPU resources before the render manager can go away, rather than at member destruction.
void ElementAvatarCrop::Release() {
  image_.Release();
  dim_.Release();
  line_.Release();
  texture_.Release();
  dim_manager_ = nullptr;
  ui::Element::Release();
}

void RegisterAvatarCropElement() {
  static ui::ElementInstancerGeneric<ElementAvatarCrop> instancer;
  ui::Factory::RegisterElementInstancer("avatar-crop", &instancer);
}

} // namespace pbr
