#include "ui/SplitLayoutHost.h"

#include "ui/PanelRegistry.h"
#include "ui/RmlMount.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <SDL3/SDL.h>

#include <cmath>
#include <sstream>
#include <string>

namespace pbr {

namespace {

std::string RatioStyle(float ratio) {
  std::ostringstream out;
  out << "flex-basis: " << static_cast<int>(ratio * 100.0f) << "%;";
  return out.str();
}

const SplitTree::Node* FindBranch(const SplitTree::Node* node, int gutter_id) {
  if (!node) {
    return nullptr;
  }
  if (node->type == SplitTree::Node::Type::Branch) {
    if (node->gutter_id == gutter_id) {
      return node;
    }
    if (const SplitTree::Node* found = FindBranch(node->first.get(), gutter_id)) {
      return found;
    }
    return FindBranch(node->second.get(), gutter_id);
  }
  return nullptr;
}

} // namespace

SplitLayoutHost& SplitLayoutHost::Instance() {
  static SplitLayoutHost host;
  return host;
}

void SplitLayoutHost::Initialize(Rml::Context* context) {
  context_ = context;
  tree_ = SplitTree::DefaultLayout();
  drag_ = {};
}

Rml::Element* SplitLayoutHost::SplitRoot() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context_->GetDocument(0)->GetElementById("split-root");
}

std::string SplitLayoutHost::SerializeNode(const SplitTree::Node* node) const {
  if (!node) {
    return {};
  }

  if (node->type == SplitTree::Node::Type::Leaf) {
    std::ostringstream out;
    out << "<div class=\"panel-leaf\" id=\"panel-" << node->panel_id << "\">";
    out << "<div class=\"panel-header\">";
    out << "<span class=\"panel-title\">" << PanelKindTitle(node->kind) << "</span>";
    out << "<button class=\"panel-btn\" data-event-click=\"split_panel_h(" << node->panel_id << ")\">|</button>";
    out << "<button class=\"panel-btn\" data-event-click=\"split_panel_v(" << node->panel_id << ")\">-</button>";
    out << "<button class=\"panel-btn\" data-event-click=\"close_panel(" << node->panel_id << ")\">x</button>";
    out << "</div>";
    out << "<div class=\"panel-body\">" << PanelRegistry::Body(node->kind) << "</div>";
    out << "</div>";
    return out.str();
  }

  const bool horizontal = node->orientation == SplitOrientation::Horizontal;
  const char* branch_class = horizontal ? "split-h" : "split-v";
  const char* gutter_class = horizontal ? "split-gutter-v" : "split-gutter-h";

  std::ostringstream out;
  out << "<div class=\"split-branch " << branch_class << "\">";
  out << "<div class=\"split-pane\" style=\"" << RatioStyle(node->ratio) << "\">" << SerializeNode(node->first.get())
      << "</div>";
  out << "<div class=\"split-gutter " << gutter_class << "\" id=\"gutter-" << node->gutter_id
      << "\" data-event-mousedown=\"gutter_drag_start(" << node->gutter_id << ")\"></div>";
  out << "<div class=\"split-pane\" style=\"flex: 1\">" << SerializeNode(node->second.get()) << "</div>";
  out << "</div>";
  return out.str();
}

std::string SplitLayoutHost::Serialize() const {
  return SerializeNode(tree_.Root());
}

void SplitLayoutHost::SyncLayout() {
  Rml::Element* root = SplitRoot();
  if (!root) {
    return;
  }
  RmlMount::MountInner(root, Serialize());
}

bool SplitLayoutHost::ApplyPanelAction(bool (SplitTree::*action)(int), int panel_id) {
  if (!(tree_.*action)(panel_id)) {
    return false;
  }
  SyncLayout();
  return true;
}

void SplitLayoutHost::SplitPanelHCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::INT) {
    return;
  }
  Instance().ApplyPanelAction(&SplitTree::SplitHorizontal, args[0].Get<int>());
}

void SplitLayoutHost::SplitPanelVCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::INT) {
    return;
  }
  Instance().ApplyPanelAction(&SplitTree::SplitVertical, args[0].Get<int>());
}

void SplitLayoutHost::ClosePanelCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                         const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::INT) {
    return;
  }
  Instance().ApplyPanelAction(&SplitTree::Close, args[0].Get<int>());
}

void SplitLayoutHost::BeginGutterDrag(int gutter_id, Rml::Element* gutter_element) {
  const SplitTree::Node* branch = FindBranch(tree_.Root(), gutter_id);
  if (!branch || !gutter_element) {
    return;
  }

  Rml::Element* branch_element = gutter_element->GetParentNode();
  if (!branch_element) {
    return;
  }

  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  SDL_GetMouseState(&mouse_x, &mouse_y);

  drag_.active = true;
  drag_.gutter_id = gutter_id;
  drag_.start_ratio = branch->ratio;
  drag_.orientation = branch->orientation;
  if (branch->orientation == SplitOrientation::Horizontal) {
    drag_.branch_size = branch_element->GetClientWidth();
    drag_.start_mouse = mouse_x;
  } else {
    drag_.branch_size = branch_element->GetClientHeight();
    drag_.start_mouse = mouse_y;
  }
  if (drag_.branch_size <= 1.0f) {
    drag_.branch_size = 1.0f;
  }
}

void SplitLayoutHost::GutterDragStartCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                              const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::INT) {
    return;
  }
  Instance().BeginGutterDrag(args[0].Get<int>(), ev.GetTargetElement());
}

void SplitLayoutHost::EndGutterDrag() {
  if (drag_.active) {
    SyncLayout();
  }
  drag_.active = false;
}

void SplitLayoutHost::GutterDragEndCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().EndGutterDrag();
}

void SplitLayoutHost::Update(Rml::Context* context) {
  (void)context;
  if (!drag_.active) {
    return;
  }

  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
  SDL_GetMouseState(&mouse_x, &mouse_y);
  const float current_mouse = drag_.orientation == SplitOrientation::Horizontal ? mouse_x : mouse_y;
  const float delta = current_mouse - drag_.start_mouse;
  const float new_ratio = drag_.start_ratio + (delta / drag_.branch_size);
  tree_.SetRatio(drag_.gutter_id, new_ratio);
}

} // namespace pbr
