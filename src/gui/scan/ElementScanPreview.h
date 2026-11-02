#pragma once

#include <ui/dom/Element.h>
#include <ui/paint/CallbackTexture.h>
#include <ui/paint/Geometry.h>

#include <cstdint>
#include <vector>

namespace pbr {

/** UI-owned camera preview. Pixels and GPU resources die with the scan surface. */
class ElementScanPreview : public ui::Element {
public:
  UI_RTTI_DefineWithParent(ElementScanPreview, ui::Element)
  explicit ElementScanPreview(const ui::String& tag);
  void SetFrame(int width, int height, std::vector<uint8_t> rgba);

protected:
  void OnRender() override;

private:
  int width_ = 0;
  int height_ = 0;
  std::vector<uint8_t> pending_;
  ui::CallbackTexture texture_;
  ui::Geometry geometry_;
  ui::Geometry frame_;      // dimming + corner marks, rebuilt only when the box or render manager changes
  ui::Geometry scan_line_;  // rebuilt every frame
  ui::Vector2f frame_box_{0, 0};
  ui::RenderManager* frame_manager_ = nullptr;
};

void RegisterScanPreviewElement();

} // namespace pbr
