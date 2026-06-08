#include "bindings/BindingsManifest.h"

#include <cassert>
#include <iostream>

int main() {
  const char* json = R"({
    "actions": {
      "search_users": {
        "tool": "user_search",
        "params": {"query": "{{input:#query}}"},
        "result_bind": "results",
        "risk": "read"
      }
    }
  })";

  pbr::BindingsManifest manifest;
  assert(pbr::BindingsManifest::Parse(json, manifest));
  const auto* action = manifest.Find("search_users");
  assert(action != nullptr);
  assert(action->tool == "user_search");
  assert(action->result_bind == "results");
  std::cout << "bindings_test ok\n";
  return 0;
}
