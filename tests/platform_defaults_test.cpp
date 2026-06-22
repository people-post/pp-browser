#include "platform/Platform.h"
#include "platform/PlatformDefaults.h"

#include <cassert>
#include <iostream>

int main() {
  const pbr::AppConfig config = pbr::PlatformDefaults::For(pbr::PlatformKind::Desktop);
  assert(config.llm.base_url == "https://api.openai.com/v1");
  assert(config.llm.require_api_key);
  assert(config.llm.model == "gpt-4o-mini");
  assert(config.search.provider == "duckduckgo");
  assert(config.theme == "themes/base.rcss");

  std::cout << "platform_defaults_test ok\n";
  return 0;
}
