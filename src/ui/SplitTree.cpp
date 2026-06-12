#include "ui/SplitTree.h"

#include <utility>

namespace pbr {

namespace {

struct LeafLocation {
  SplitTree::Node* leaf = nullptr;
  SplitTree::Node* parent_branch = nullptr;
  SplitTree::Node* grandparent_branch = nullptr;
  bool is_first_child = true;
  bool parent_is_first_child = true;
};

std::unique_ptr<SplitTree::Node> MakeLeaf(int panel_id, PanelKind kind) {
  auto node = std::make_unique<SplitTree::Node>();
  node->type = SplitTree::Node::Type::Leaf;
  node->panel_id = panel_id;
  node->kind = kind;
  return node;
}

std::unique_ptr<SplitTree::Node> MakeBranch(SplitOrientation orientation, int gutter_id, float ratio,
                                            std::unique_ptr<SplitTree::Node> first,
                                            std::unique_ptr<SplitTree::Node> second) {
  auto node = std::make_unique<SplitTree::Node>();
  node->type = SplitTree::Node::Type::Branch;
  node->orientation = orientation;
  node->gutter_id = gutter_id;
  node->ratio = ratio;
  node->first = std::move(first);
  node->second = std::move(second);
  return node;
}

std::optional<LeafLocation> FindLeafRecursive(SplitTree::Node* node, SplitTree::Node* parent_branch,
                                              SplitTree::Node* grandparent_branch, bool is_first_child,
                                              bool parent_is_first_child, int panel_id) {
  if (!node) {
    return std::nullopt;
  }
  if (node->type == SplitTree::Node::Type::Leaf && node->panel_id == panel_id) {
    return LeafLocation{node, parent_branch, grandparent_branch, is_first_child, parent_is_first_child};
  }
  if (node->type == SplitTree::Node::Type::Branch) {
    if (auto found = FindLeafRecursive(node->first.get(), node, parent_branch, true, is_first_child, panel_id)) {
      return found;
    }
    return FindLeafRecursive(node->second.get(), node, parent_branch, false, is_first_child, panel_id);
  }
  return std::nullopt;
}

std::optional<SplitTree::Node*> FindGutterRecursive(SplitTree::Node* node, int gutter_id) {
  if (!node) {
    return std::nullopt;
  }
  if (node->type == SplitTree::Node::Type::Branch) {
    if (node->gutter_id == gutter_id) {
      return node;
    }
    if (auto found = FindGutterRecursive(node->first.get(), gutter_id)) {
      return found;
    }
    return FindGutterRecursive(node->second.get(), gutter_id);
  }
  return std::nullopt;
}

int CountLeaves(const SplitTree::Node* node) {
  if (!node) {
    return 0;
  }
  if (node->type == SplitTree::Node::Type::Leaf) {
    return 1;
  }
  return CountLeaves(node->first.get()) + CountLeaves(node->second.get());
}

} // namespace

const char* PanelKindTitle(PanelKind kind) {
  switch (kind) {
  case PanelKind::Sidebar:
    return "Sessions";
  case PanelKind::Chat:
    return "Chat";
  case PanelKind::Preview:
    return "Preview";
  case PanelKind::Empty:
    return "Empty";
  }
  return "Panel";
}

SplitTree::SplitTree() = default;

SplitTree SplitTree::DefaultLayout() {
  SplitTree tree;
  const int sidebar_id = tree.AllocatePanelId();
  const int chat_id = tree.AllocatePanelId();
  const int preview_id = tree.AllocatePanelId();
  const int inner_gutter = tree.AllocateGutterId();
  const int outer_gutter = tree.AllocateGutterId();

  auto inner = MakeBranch(SplitOrientation::Horizontal, inner_gutter, 0.72f, MakeLeaf(chat_id, PanelKind::Chat),
                          MakeLeaf(preview_id, PanelKind::Preview));
  tree.root_ =
      MakeBranch(SplitOrientation::Horizontal, outer_gutter, 0.22f, MakeLeaf(sidebar_id, PanelKind::Sidebar), std::move(inner));
  return tree;
}

int SplitTree::AllocatePanelId() { return next_panel_id_++; }

int SplitTree::AllocateGutterId() { return next_gutter_id_++; }

float SplitTree::ClampRatio(float ratio) const {
  if (ratio < kMinRatio) {
    return kMinRatio;
  }
  if (ratio > kMaxRatio) {
    return kMaxRatio;
  }
  return ratio;
}

int SplitTree::LeafCount() const { return CountLeaves(root_.get()); }

std::optional<SplitTree::NodeLocation> SplitTree::FindLeaf(int panel_id) {
  auto found = FindLeafRecursive(root_.get(), nullptr, nullptr, true, true, panel_id);
  if (!found) {
    return std::nullopt;
  }
  return NodeLocation{found->leaf, found->parent_branch, found->grandparent_branch, found->is_first_child,
                      found->parent_is_first_child};
}

bool SplitTree::ReplaceChild(Node* parent, bool first_child, std::unique_ptr<Node> replacement) {
  if (!parent || parent->type != Node::Type::Branch) {
    return false;
  }
  if (first_child) {
    parent->first = std::move(replacement);
  } else {
    parent->second = std::move(replacement);
  }
  return true;
}

std::unique_ptr<SplitTree::Node> SplitTree::DetachChild(Node* parent, bool first_child) {
  if (!parent || parent->type != Node::Type::Branch) {
    return nullptr;
  }
  if (first_child) {
    return std::move(parent->first);
  }
  return std::move(parent->second);
}

bool SplitTree::SplitHorizontal(int panel_id) {
  auto location = FindLeaf(panel_id);
  if (!location || !location->leaf) {
    return false;
  }

  auto leaf = std::make_unique<Node>();
  leaf->type = Node::Type::Leaf;
  leaf->panel_id = location->leaf->panel_id;
  leaf->kind = location->leaf->kind;

  const int empty_id = AllocatePanelId();
  const int gutter_id = AllocateGutterId();
  auto branch = MakeBranch(SplitOrientation::Horizontal, gutter_id, 0.5f, std::move(leaf),
                           MakeLeaf(empty_id, PanelKind::Empty));

  if (location->parent_branch) {
    return ReplaceChild(location->parent_branch, location->is_first_child, std::move(branch));
  }
  root_ = std::move(branch);
  return true;
}

bool SplitTree::SplitVertical(int panel_id) {
  auto location = FindLeaf(panel_id);
  if (!location || !location->leaf) {
    return false;
  }

  auto leaf = std::make_unique<Node>();
  leaf->type = Node::Type::Leaf;
  leaf->panel_id = location->leaf->panel_id;
  leaf->kind = location->leaf->kind;

  const int empty_id = AllocatePanelId();
  const int gutter_id = AllocateGutterId();
  auto branch = MakeBranch(SplitOrientation::Vertical, gutter_id, 0.5f, std::move(leaf),
                           MakeLeaf(empty_id, PanelKind::Empty));

  if (location->parent_branch) {
    return ReplaceChild(location->parent_branch, location->is_first_child, std::move(branch));
  }
  root_ = std::move(branch);
  return true;
}

bool SplitTree::Close(int panel_id) {
  if (LeafCount() <= 1) {
    return false;
  }

  auto location = FindLeaf(panel_id);
  if (!location || !location->parent_branch) {
    return false;
  }

  auto promoted = DetachChild(location->parent_branch, !location->is_first_child);
  if (!promoted) {
    return false;
  }

  if (location->grandparent_branch) {
    return ReplaceChild(location->grandparent_branch, location->parent_is_first_child, std::move(promoted));
  }
  root_ = std::move(promoted);
  return true;
}

bool SplitTree::SetRatio(int gutter_id, float ratio) {
  if (auto branch = FindGutterRecursive(root_.get(), gutter_id)) {
    (*branch)->ratio = ClampRatio(ratio);
    return true;
  }
  return false;
}

} // namespace pbr
