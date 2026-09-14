#pragma once

#include <ui/base/Types.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace ui {
class Element;
class ElementDocument;
}

namespace pbr {

struct MountOptions {
  bool validate = true;
  bool preserve_focus = true;
  bool preserve_scroll = true;
};

struct MountState {
  ui::String focused_id;
  std::vector<std::pair<ui::String, ui::Vector2f>> scroll_positions;
};

class RmlMount {
public:
  static bool MountInner(ui::Element* target, const std::string& rml, MountOptions opts = {});

  static bool InjectRcss(ui::ElementDocument* doc, const std::string& rcss, const std::string& source_tag = "dynamic");

  static MountState CaptureState(ui::Element* subtree);
  static void RestoreState(ui::Element* subtree, const MountState& state);

  static void ClearDocumentStyleState(ui::ElementDocument* doc);

private:
  struct DocumentStyleState;

  static bool IsDescendantOf(ui::Element* ancestor, ui::Element* node);
  static void CollectScrollState(ui::Element* element, MountState& state);
  static void RestoreScrollState(ui::Element* element, const MountState& state);
  static bool ReapplyInjectedStyles(ui::ElementDocument* doc, DocumentStyleState& state);

  static std::unordered_map<ui::ElementDocument*, DocumentStyleState> style_state_;
};

} // namespace pbr
