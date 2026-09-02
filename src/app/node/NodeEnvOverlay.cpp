#include "app/node/NodeEnvOverlay.h"

#include <cctype>
#include <cstdlib>
#include <string>

namespace pbr {
namespace {

const char* EnvOrNull(const char* name) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') {
    return nullptr;
  }
  return v;
}

std::string Trim(std::string_view in) {
  size_t begin = 0;
  while (begin < in.size() && std::isspace(static_cast<unsigned char>(in[begin]))) {
    ++begin;
  }
  size_t end = in.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(in[end - 1]))) {
    --end;
  }
  return std::string(in.substr(begin, end - begin));
}

std::string ToLowerAscii(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

} // namespace

std::optional<bool> ParsePpNodeBoolEnv(std::string_view value) {
  const std::string v = ToLowerAscii(Trim(value));
  if (v.empty()) {
    return std::nullopt;
  }
  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    return true;
  }
  if (v == "0" || v == "false" || v == "no" || v == "off") {
    return false;
  }
  return std::nullopt;
}

std::vector<std::string> ParsePpNodeBootstrapPeersCsv(std::string_view csv) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= csv.size()) {
    const size_t comma = csv.find(',', start);
    const std::string_view token =
        comma == std::string_view::npos ? csv.substr(start) : csv.substr(start, comma - start);
    std::string peer = Trim(token);
    if (!peer.empty()) {
      out.push_back(std::move(peer));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  return out;
}

void ApplyPpNodeConfigEnvOverlays(AppConfig& config) {
  if (const char* data_dir = EnvOrNull("PP_NODE_DATA_DIR")) {
    config.data_dir = data_dir;
  }
  if (const char* peers = EnvOrNull("PP_NODE_BOOTSTRAP_PEERS")) {
    config.mesh.bootstrap_peers = ParsePpNodeBootstrapPeersCsv(peers);
  }
  if (const char* amp_udp = EnvOrNull("PP_NODE_AMP_UDP_PORT")) {
    try {
      const int port = std::stoi(amp_udp);
      if (port >= 0 && port <= 65535) {
        config.mesh.amp_udp_port = port;
      }
    } catch (...) {
    }
  }
  if (const char* circuit = EnvOrNull("PP_NODE_CAP_CIRCUIT_RELAY")) {
    if (auto parsed = ParsePpNodeBoolEnv(circuit)) {
      config.mesh.capabilities.circuit_relay = *parsed;
    }
  }
  if (const char* media = EnvOrNull("PP_NODE_CAP_MEDIA_RELAY")) {
    if (auto parsed = ParsePpNodeBoolEnv(media)) {
      config.mesh.capabilities.media_relay = *parsed;
    }
  }
  if (const char* dht = EnvOrNull("PP_NODE_CAP_DHT")) {
    if (auto parsed = ParsePpNodeBoolEnv(dht)) {
      config.mesh.capabilities.dht = *parsed;
    }
  }
  if (const char* advertise = EnvOrNull("PP_NODE_ADVERTISE_MULTIADDRS")) {
    config.mesh.advertise_multiaddrs = ParsePpNodeBootstrapPeersCsv(advertise);
  }
  if (const char* mesh_publish = EnvOrNull("PP_NODE_MESH_PUBLISH")) {
    if (auto parsed = ParsePpNodeBoolEnv(mesh_publish)) {
      config.mesh.mesh_publish = *parsed;
    }
  } else if (!config.mesh.advertise_multiaddrs.empty()) {
    // Convenience: advertise list implies mesh_node publish for pp-node (N027).
    config.mesh.mesh_publish = true;
  }
  if (const char* registration = EnvOrNull("PP_NODE_REGISTRATION_BASE_URL")) {
    config.registration.base_url = registration;
  }
}

} // namespace pbr
