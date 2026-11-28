#include "app/node/NodeServiceAsks.h"

#include "app/node/NodeEnvOverlay.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace pbr {
namespace {

bool Contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

std::string Join(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    out += (out.empty() ? "" : ", ") + name;
  }
  return out.empty() ? "none" : out;
}

} // namespace

void ApplyPpNodeServiceEnvOverlays(NodeConfig& config, const std::vector<std::string>& known) {
  for (const std::string& service : known) {
    if (service == "ledger") {
      continue;
    }
    std::string name = "PP_NODE_";
    for (const char c : service) {
      name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    name += "_ENABLED";
    const char* text = std::getenv(name.c_str());
    if (!text || !*text) {
      continue;
    }
    if (const auto enabled = ParsePpNodeBoolEnv(text)) {
      Object section;
      if (const Object* existing = config.node.getObject(service)) {
        section = *existing;
      }
      section.set("enabled", *enabled);
      config.node.set(service, section);
    }
  }
}

Roe<std::vector<std::string>> CheckPpNodeServiceAsks(const NodeConfig& config, const std::vector<std::string>& built,
                                                     const std::vector<std::string>& known) {
  std::vector<std::string> ignored;
  for (const std::string& service : known) {
    if (service == "ledger" || Contains(built, service)) {
      continue;
    }
    const Object* section = config.node.getObject(service);
    if (!section) {
      continue;
    }
    if (section->getIf<bool>("enabled").value_or(false)) {
      return Error("node." + service + ".enabled is on, but this build has no " + service +
                   " (its services: " + Join(built) + "); run a build with it (pp-node-plus), or turn it off");
    }
    ignored.push_back(service);
  }
  return ignored;
}

} // namespace pbr
