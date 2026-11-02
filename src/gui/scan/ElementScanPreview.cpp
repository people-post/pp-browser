#include "gui/scan/ElementScanPreview.h"

#include <ui/dom/ElementInstancer.h>
#include <ui/dom/Factory.h>
#include <ui/paint/MeshUtilities.h>
#include <ui/paint/RenderManager.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pbr {
namespace {
float ViewfinderSide(ui::Vector2f box) { return std::min(box.x, box.y) * .68f; }

// Dimming around the finder and its four corner marks: they depend on the box size only.
ui::Mesh ViewfinderFrameMesh(ui::Vector2f box) {
  ui::Mesh mesh;
  const float side = ViewfinderSide(box);
  const ui::Vector2f corner((box.x - side) * .5f, (box.y - side) * .5f);
  const ui::ColourbPremultiplied shade(0, 0, 0, 140);
  const ui::ColourbPremultiplied white(255, 255, 255, 255);
  auto quad = [&](ui::Vector2f at, ui::Vector2f size, ui::ColourbPremultiplied color) {
    ui::MeshUtilities::GenerateQuad(mesh, at, size, color);
  };
  quad({0, 0}, {box.x, corner.y}, shade);
  quad({0, corner.y + side}, {box.x, corner.y}, shade);
  quad({0, corner.y}, {corner.x, side}, shade);
  quad({corner.x + side, corner.y}, {corner.x, side}, shade);
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const ui::Vector2f at(corner.x + x * (side - 3.f), corner.y + y * (side - 3.f));
      quad({corner.x + x * (side - 18.f), at.y}, {18.f, 3.f}, white);
      quad({at.x, corner.y + y * (side - 18.f)}, {3.f, 18.f}, white);
    }
  }
  return mesh;
}

// The one moving part: the scan line.
ui::Mesh ScanLineMesh(ui::Vector2f box) {
  ui::Mesh mesh;
  const float side = ViewfinderSide(box);
  const ui::Vector2f corner((box.x - side) * .5f, (box.y - side) * .5f);
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const float phase = static_cast<float>(std::abs(std::fmod(seconds, 2.4) / 1.2 - 1.0));
  ui::MeshUtilities::GenerateQuad(mesh, {corner.x + 6.f, corner.y + 6.f + (side - 14.f) * phase},
                                  {side - 12.f, 2.f}, ui::ColourbPremultiplied(74, 108, 247, 255));
  return mesh;
}
} // namespace

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
  const float scale = std::max(box.x / width_, box.y / height_);
  const ui::Vector2f size(width_ * scale, height_ * scale);
  const ui::Vector2f inset((box.x - size.x) * .5f, (box.y - size.y) * .5f);
  ui::Mesh mesh;
  ui::MeshUtilities::GenerateQuad(mesh, inset, size, ui::ColourbPremultiplied(255, 255, 255, 255));
  geometry_ = manager->MakeGeometry(std::move(mesh));
  geometry_.Render(GetAbsoluteOffset(ui::BoxArea::Content), texture_);
  if (manager != frame_manager_ || box != frame_box_) {
    frame_manager_ = manager;
    frame_box_ = box;
    frame_ = manager->MakeGeometry(ViewfinderFrameMesh(box));
  }
  frame_.Render(GetAbsoluteOffset(ui::BoxArea::Content));
  scan_line_ = manager->MakeGeometry(ScanLineMesh(box));
  scan_line_.Render(GetAbsoluteOffset(ui::BoxArea::Content));
}

// Pixel sizes derive from the dp ratio, so the cached finder geometry is stale after it changes.
void ElementScanPreview::OnDpRatioChange() {
  ui::Element::OnDpRatioChange();
  frame_manager_ = nullptr;
}

// Drop the GPU resources before the render manager can go away, rather than at member destruction.
void ElementScanPreview::Release() {
  frame_.Release();
  scan_line_.Release();
  geometry_.Release();
  frame_manager_ = nullptr;
  ui::Element::Release();
}

void RegisterScanPreviewElement() {
  static ui::ElementInstancerGeneric<ElementScanPreview> instancer;
  ui::Factory::RegisterElementInstancer("scan-preview", &instancer);
}

} // namespace pbr
