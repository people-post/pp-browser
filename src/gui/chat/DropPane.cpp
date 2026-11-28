#include "gui/chat/DropPane.h"

#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>

namespace pbr {

bool DropPaneContains(ui::ElementDocument* document, const LayoutMode layout_mode, const DropUiPoint point) {
  if (!document) {
    return false;
  }
  ui::Element* pane =
      document->GetElementById(layout_mode == LayoutMode::Expanded ? "pane-chat" : "shell-chat-overlay");
  if (!pane || !pane->IsVisible()) {
    return false;
  }
  const ui::Vector2f origin = pane->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = pane->GetBox().GetSize(ui::BoxArea::Border);
  const float x = static_cast<float>(point.x);
  const float y = static_cast<float>(point.y);
  return x >= origin.x && x < origin.x + size.x && y >= origin.y && y < origin.y + size.y;
}

} // namespace pbr
