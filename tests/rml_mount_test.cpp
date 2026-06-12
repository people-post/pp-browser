#include "agent/RmlValidator.h"

#include <cassert>
#include <iostream>

int main() {
  const std::string valid_fragment =
      R"frag(<div class="stack"><button data-event-click="go()">Go</button></div>)frag";
  auto ok = pbr::RmlValidator::ValidateFragment(valid_fragment);
  assert(ok.ok);

  const std::string bad_fragment = R"bad(<div onclick="alert(1)"></div>)bad";
  auto bad_result = pbr::RmlValidator::ValidateFragment(bad_fragment);
  assert(!bad_result.ok);

  const std::string document_without_root = "<body><p>Hi</p></body>";
  auto doc_result = pbr::RmlValidator::ValidateRml(document_without_root);
  assert(!doc_result.ok);

  const std::string valid_document = "<rml><body><p>Hi</p></body></rml>";
  assert(pbr::RmlValidator::ValidateRml(valid_document).ok);

  std::cout << "rml_mount_test ok\n";
  return 0;
}
