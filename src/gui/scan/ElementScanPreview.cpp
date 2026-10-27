#include "gui/scan/ElementScanPreview.h"

#include <ui/dom/ElementInstancer.h>
#include <ui/dom/Factory.h>
#include <ui/paint/MeshUtilities.h>
#include <ui/paint/RenderManager.h>

#include <algorithm>

namespace pbr {

ElementScanPreview::ElementScanPreview(const ui::String& tag) : ui::Element(tag) {}

void ElementScanPreview::SetFrame(int width, int height, std::vector<uint8_t> rgba) {
  if (width <= 0 || height <= 0 || rgba.size() != static_cast<size_t>(width) * height * 4) {
    return;
  }
  width_ = width;
  height_ = height;
  pending_ = std::move(rgba);
}

void ElementScanPreview::OnRender() {
  auto* manager = GetRenderManager();
  if (!manager || width_ <= 0 || height_ <= 0) {
    return;
  }
  if (!pending_.empty()) {
    texture_ = manager->MakeCallbackTexture(
        [pixels = std::move(pending_), dimensions = ui::Vector2i(width_, height_)](
            const ui::CallbackTextureInterface& target) {
          return target.GenerateTexture({pixels.data(), pixels.size()}, dimensions);
        });
    pending_.clear();
  }
  const ui::Vector2f box = GetBox().GetSize(ui::BoxArea::Content);
  if (box.x <= 0 || box.y <= 0) {
    return;
  }
  const float scale = std::min(box.x / width_, box.y / height_);
  const ui::Vector2f size(width_ * scale, height_ * scale);
  const ui::Vector2f inset((box.x - size.x) * .5f, (box.y - size.y) * .5f);
  ui::Mesh mesh;
  ui::MeshUtilities::GenerateQuad(mesh, inset, size, ui::ColourbPremultiplied(255, 255, 255, 255));
  geometry_ = manager->MakeGeometry(std::move(mesh));
  geometry_.Render(GetAbsoluteOffset(ui::BoxArea::Content), texture_);
}

void RegisterScanPreviewElement() {
  static ui::ElementInstancerGeneric<ElementScanPreview> instancer;
  ui::Factory::RegisterElementInstancer("scan-preview", &instancer);
}

} // namespace pbr
