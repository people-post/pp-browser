#include "base/ai/RmlValidator.h"

#include <cassert>
#include <iostream>

int main() {
  const std::string valid = "<rml><body><button data-event-click=\"go()\"/></body></rml>";
  auto ok = pbr::RmlValidator::ValidateRml(valid);
  assert(ok.ok);

  const std::string bad = "<rml><script>alert(1)</script></rml>";
  auto bad_result = pbr::RmlValidator::ValidateRml(bad);
  assert(!bad_result.ok);

  std::cout << "rml_validator_test ok\n";
  return 0;
}
