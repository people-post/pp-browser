#pragma once

#include <memory>
#include <optional>

namespace pbr {

enum class PanelKind {
  Sidebar,
  Chat,
  Preview,
  Empty,
};

enum class SplitOrientation {
  Horizontal,
  Vertical,
};

class SplitTree {
public:
  static constexpr float kMinRatio = 0.15f;
  static constexpr float kMaxRatio = 0.85f;

  struct Node {
    enum class Type { Leaf, Branch } type = Type::Leaf;
    PanelKind kind = PanelKind::Empty;
    int panel_id = 0;
    SplitOrientation orientation = SplitOrientation::Horizontal;
    int gutter_id = 0;
    float ratio = 0.5f;
    std::unique_ptr<Node> first;
    std::unique_ptr<Node> second;
  };

  SplitTree();

  static SplitTree DefaultLayout();

  int LeafCount() const;
  bool SplitHorizontal(int panel_id);
  bool SplitVertical(int panel_id);
  bool Close(int panel_id);
  bool SetRatio(int gutter_id, float ratio);

  const Node* Root() const { return root_.get(); }

private:
  struct NodeLocation {
    Node* leaf = nullptr;
    Node* parent_branch = nullptr;
    Node* grandparent_branch = nullptr;
    bool is_first_child = true;
    bool parent_is_first_child = true;
  };

  int AllocatePanelId();
  int AllocateGutterId();
  float ClampRatio(float ratio) const;
  std::optional<NodeLocation> FindLeaf(int panel_id);
  bool ReplaceChild(Node* parent, bool first_child, std::unique_ptr<Node> replacement);
  std::unique_ptr<Node> DetachChild(Node* parent, bool first_child);

  int next_panel_id_ = 1;
  int next_gutter_id_ = 1;
  std::unique_ptr<Node> root_;
};

const char* PanelKindTitle(PanelKind kind);

} // namespace pbr
