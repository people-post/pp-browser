#pragma once

#include "ui/SplitTree.h"

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Types.h>

namespace Rml {
class Context;
class Element;
}

namespace pbr {

class SplitLayoutHost {
public:
  static SplitLayoutHost& Instance();

  void Initialize(Rml::Context* context);
  void SyncLayout();
  void Update(Rml::Context* context);

  SplitTree& Tree() { return tree_; }
  const SplitTree& Tree() const { return tree_; }

  static void SplitPanelHCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void SplitPanelVCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void ClosePanelCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void GutterDragStartCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);
  static void GutterDragEndCallback(Rml::DataModelHandle model, Rml::Event& ev, const Rml::VariantList& args);

private:
  SplitLayoutHost() = default;

  std::string Serialize() const;
  std::string SerializeNode(const SplitTree::Node* node) const;
  Rml::Element* SplitRoot() const;
  bool ApplyPanelAction(bool (SplitTree::*action)(int), int panel_id);
  void BeginGutterDrag(int gutter_id, Rml::Element* gutter_element);
  void EndGutterDrag();

  Rml::Context* context_ = nullptr;
  SplitTree tree_ = SplitTree::DefaultLayout();

  struct DragState {
    bool active = false;
    int gutter_id = 0;
    float start_ratio = 0.5f;
    float start_mouse = 0.0f;
    float branch_size = 1.0f;
    SplitOrientation orientation = SplitOrientation::Horizontal;
  } drag_;
};

} // namespace pbr
