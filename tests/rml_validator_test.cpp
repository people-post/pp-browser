#include "agent/RmlValidator.h"

#include <cassert>
#include <iostream>

int main() {
  const std::string valid = "<rml><body><button data-event-click=\"go()\"/></body></rml>";
  auto ok = ppbrowser::RmlValidator::ValidateRml(valid);
  assert(ok.ok);

  const std::string bad = "<rml><script>alert(1)</script></rml>";
  auto bad_result = ppbrowser::RmlValidator::ValidateRml(bad);
  assert(!bad_result.ok);

  const std::string bindings = R"({"actions":{"go":{"tool":"ping"}}})";
  assert(ppbrowser::RmlValidator::ValidateBindings(bindings).ok);

  std::cout << "rml_validator_test ok\n";
  return 0;
}
