#include "base/data/ConfigJson.h"

#include <cassert>
#include <iostream>
#include <nlohmann/json.hpp>

int main() {
  const nlohmann::json legacy = nlohmann::json::parse(R"({
    "mcp": { "url": "https://legacy.example/mcp" },
    "mcp_servers": [
      { "id": "custom-a", "url": "https://custom.example/mcp" }
    ]
  })");

  pbr::AppConfig config;
  pbr::from_json(legacy, config);
  assert(config.promoted_mcp.url == "https://legacy.example/mcp");
  assert(config.mcp_servers.size() == 1);
  assert(config.mcp_servers[0].id == "custom-a");

  const nlohmann::json modern = nlohmann::json::parse(R"({
    "promoted_mcp": { "url": "https://promoted.example/mcp" },
    "mcp_servers": [
      { "id": "b", "command": "mock", "enabled": true }
    ],
    "relay": { "base_url": "https://relay.example" },
    "directory": { "base_url": "" },
    "registration": { "base_url": "" }
  })");
  pbr::from_json(modern, config);
  assert(config.promoted_mcp.url == "https://promoted.example/mcp");
  assert(config.mcp_servers.size() == 1);
  assert(config.mcp_servers[0].command == "mock");
  assert(config.relay.base_url == "https://relay.example");

  nlohmann::json out;
  pbr::to_json(out, config);
  assert(out.contains("promoted_mcp"));
  assert(!out.contains("mcp"));

  pbr::AppConfig defaults;
  defaults.promoted_mcp.url = "https://www.brief.global/mcp";
  assert(pbr::ResolvePromotedMcp(defaults, defaults).url == "https://www.brief.global/mcp");
  pbr::AppConfig empty_promoted;
  assert(pbr::ResolvePromotedMcp(empty_promoted, defaults).url == "https://www.brief.global/mcp");

  std::cout << "config_json_test ok\n";
  return 0;
}
