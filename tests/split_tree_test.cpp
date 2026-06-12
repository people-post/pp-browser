#include "ui/SplitTree.h"

#include <cassert>
#include <iostream>

int main() {
  pbr::SplitTree tree = pbr::SplitTree::DefaultLayout();
  assert(tree.LeafCount() == 3);

  assert(tree.SplitVertical(2));
  assert(tree.LeafCount() == 4);

  assert(tree.Close(4));
  assert(tree.LeafCount() == 3);

  assert(tree.SetRatio(1, 0.30f));
  assert(!tree.SetRatio(999, 0.5f));

  assert(tree.Close(1));
  assert(tree.LeafCount() == 2);
  assert(tree.Close(3));
  assert(tree.LeafCount() == 1);
  assert(!tree.Close(2));

  std::cout << "split_tree_test ok\n";
  return 0;
}
