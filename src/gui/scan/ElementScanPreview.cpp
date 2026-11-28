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

// Dimming around the finder and its four corner marks: they depend on the box size only.
ui::Mesh ViewfinderFrameMesh(ui::Vector2f box, float side) {
  ui::Mesh mesh;
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
ui::Mesh ScanLineMesh(ui::Vector2f box, float side) {
  ui::Mesh mesh;
  const ui::Vector2f corner((box.x - side) * .5f, (box.y - side) * .5f);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  const float phase = static_cast<float>(std::abs(std::fmod(seconds, 2.4) / 1.2 - 1.0));
  ui::MeshUtilities::GenerateQuad(mesh, {corner.x + 6.f, corner.y + 6.f + (side - 14.f) * phase}, {side - 12.f, 2.f},
                                  ui::ColourbPremultiplied(74, 108, 247, 255));
  return mesh;
}
} // namespace

ScanPreviewLayout LayoutScanPreview(const ui::Vector2f box, const int frame_width, const int frame_height,
                                    const bool whole_frame) {
  ScanPreviewLayout layout;
  if (frame_width <= 0 || frame_height <= 0) {
    return layout;
  }
  const float scale = whole_frame ? std::min(box.x / frame_width, box.y / frame_height)
                                  : std::max(box.x / frame_width, box.y / frame_height);
  layout.image_size = ui::Vector2f(frame_width * scale, frame_height * scale);
  layout.image_offset = ui::Vector2f((box.x - layout.image_size.x) * .5f, (box.y - layout.image_size.y) * .5f);
  layout.guide_side = whole_frame ? std::min(layout.image_size.x, layout.image_size.y) * kScanGuideShare
                                  : std::min(box.x, box.y) * .68f;
  return layout;
}

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
  // A computer's scan layer shows the whole frame, aspect-fit, the guide on the picture, mirrored when asked; the phone's
  // layer and the framed panel keep the covering picture. (The guide on the phone is drawn by the view.)
  const bool computer = Closest(".shell-layer-computer") != nullptr;
  const ScanPreviewLayout layout = LayoutScanPreview(box, width_, height_, computer);
  const bool mirrored = computer && mirrored_;
  ui::Mesh mesh;
  ui::MeshUtilities::GenerateQuad(mesh, layout.image_offset, layout.image_size,
                                  ui::ColourbPremultiplied(255, 255, 255, 255), ui::Vector2f(mirrored ? 1.f : 0.f, 0.f),
                                  ui::Vector2f(mirrored ? 0.f : 1.f, 1.f));
  geometry_ = manager->MakeGeometry(std::move(mesh));
  geometry_.Render(GetAbsoluteOffset(ui::BoxArea::Content), texture_);
  // Phones draw the viewfinder in the view instead (qr_scan.rml: a fixed square, its corners and a moving line over
  // the full-screen camera); the framed panel and a computer's layer draw this one, on the picture.
  if (Closest(".shell-layer-compact")) {
    return;
  }
  const ui::Vector2f finder_box = computer ? layout.image_size : box;
  const ui::Vector2f finder_at = computer ? layout.image_offset : ui::Vector2f(0, 0);
  if (manager != frame_manager_ || finder_box != frame_box_) {
    frame_manager_ = manager;
    frame_box_ = finder_box;
    frame_ = manager->MakeGeometry(
        ViewfinderFrameMesh(finder_box, computer ? layout.guide_side : std::min(box.x, box.y) * .68f));
  }
  frame_.Render(GetAbsoluteOffset(ui::BoxArea::Content) + finder_at);
  scan_line_ =
      manager->MakeGeometry(ScanLineMesh(finder_box, computer ? layout.guide_side : std::min(box.x, box.y) * .68f));
  scan_line_.Render(GetAbsoluteOffset(ui::BoxArea::Content) + finder_at);
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
