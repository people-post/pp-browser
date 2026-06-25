#pragma once

#include <RmlUi/Core/Types.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace Rml {
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
  Rml::String focused_id;
  std::vector<std::pair<Rml::String, Rml::Vector2f>> scroll_positions;
};

class RmlMount {
public:
  static bool MountInner(Rml::Element* target, const std::string& rml, MountOptions opts = {});

  static bool InjectRcss(Rml::ElementDocument* doc, const std::string& rcss, const std::string& source_tag = "dynamic");

  static MountState CaptureState(Rml::Element* subtree);
  static void RestoreState(Rml::Element* subtree, const MountState& state);

  static void ClearDocumentStyleState(Rml::ElementDocument* doc);

private:
  struct DocumentStyleState;

  static bool IsDescendantOf(Rml::Element* ancestor, Rml::Element* node);
  static void CollectScrollState(Rml::Element* element, MountState& state);
  static void RestoreScrollState(Rml::Element* element, const MountState& state);
  static bool ReapplyInjectedStyles(Rml::ElementDocument* doc, DocumentStyleState& state);

  static std::unordered_map<Rml::ElementDocument*, DocumentStyleState> style_state_;
};

} // namespace pbr
