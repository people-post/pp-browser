#include "foundation/data/ConfigSectionsJson.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

std::optional<int64_t> ReadI64(const Object& object, const char* key) {
  if (auto value = object.getIf<int64_t>(key)) {
    return *value;
  }
  if (auto value = object.getNonNegInt(key)) {
    if (*value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return static_cast<int64_t>(*value);
    }
  }
  return std::nullopt;
}

Object RelayPricingToObject(const mesh::RelayPricingConfig& pricing) {
  Object object;
  object.set("mode", pricing.mode);
  object.set("rate", pricing.rate);
  return object;
}

void RelayPricingFromObject(const Object& object, mesh::RelayPricingConfig& pricing) {
  if (auto mode = object.getString("mode")) {
    pricing.mode = *mode;
  }
  if (auto rate = object.getIf<double>("rate")) {
    pricing.rate = *rate;
  } else if (auto rate_i = ReadI64(object, "rate")) {
    pricing.rate = static_cast<double>(*rate_i);
  }
}

Object MeshPricingToObject(const mesh::MeshPricingConfig& pricing) {
  Object object;
  object.set("media_relay", RelayPricingToObject(pricing.media_relay));
  return object;
}

void MeshPricingFromObject(const Object& object, mesh::MeshPricingConfig& pricing) {
  if (const Object* media_relay = object.getObject("media_relay")) {
    RelayPricingFromObject(*media_relay, pricing.media_relay);
  }
}

Object MediaRelayBudgetToObject(const mesh::MediaRelayBudgetConfig& budget) {
  auto set_bps = [](Object& object, const char* key, int64_t value) {
    if (value <= 0) {
      object.set(key, Null{});
    } else {
      object.set(key, value);
    }
  };
  Object object;
  set_bps(object, "node_capacity_up_bps", budget.node_capacity_up_bps);
  set_bps(object, "node_capacity_down_bps", budget.node_capacity_down_bps);
  set_bps(object, "max_session_up_bps", budget.max_session_up_bps);
  set_bps(object, "max_session_down_bps", budget.max_session_down_bps);
  set_bps(object, "default_per_user_up_bps", budget.default_per_user_up_bps);
  set_bps(object, "default_per_user_down_bps", budget.default_per_user_down_bps);
  return object;
}

Object MediaRelayVideoToObject(const mesh::MediaRelayVideoConfig& video) {
  std::vector<Value> levels;
  for (const int level : video.serve_levels) {
    levels.emplace_back(int64_t{level});
  }
  Object object;
  object.set("serve_levels", makeArray(std::move(levels)));
  object.set("carry_levels", int64_t{video.carry_levels});
  object.set("strict", video.strict);
  return object;
}

void MediaRelayVideoFromObject(const Object& object, mesh::MediaRelayVideoConfig& video) {
  if (const Array* levels = object.getArray("serve_levels")) {
    video.serve_levels.clear();
    for (const Value& row : levels->elements) {
      if (const auto* level = std::get_if<int64_t>(&row)) {
        video.serve_levels.push_back(static_cast<int>(*level));
      }
    }
  }
  if (auto carry = ReadI64(object, "carry_levels")) {
    video.carry_levels = static_cast<int>(*carry);
  }
  if (auto strict = object.getIf<bool>("strict")) {
    video.strict = *strict;
  }
}

Object MeshCapacityToObject(const mesh::MeshCapacityConfig& capacity) {
  auto set_count = [](Object& object, const char* key, size_t value) {
    if (value == 0) {
      object.set(key, Null{});
    } else {
      object.set(key, static_cast<int64_t>(value));
    }
  };
  Object object;
  set_count(object, "links", capacity.links);
  set_count(object, "reservations", capacity.reservations);
  set_count(object, "bridges", capacity.bridges);
  set_count(object, "group_calls", capacity.group_calls);
  return object;
}

/** Null, absent, zero or negative keeps the default (0). */
void MeshCapacityFromObject(const Object& object, mesh::MeshCapacityConfig& capacity) {
  auto read_count = [&](const char* key, size_t& out) {
    if (!object.contains(key) || object.isNull(key)) {
      return;
    }
    if (auto value = ReadI64(object, key); value && *value > 0) {
      out = static_cast<size_t>(*value);
    }
  };
  read_count("links", capacity.links);
  read_count("reservations", capacity.reservations);
  read_count("bridges", capacity.bridges);
  read_count("group_calls", capacity.group_calls);
}

void MediaRelayBudgetFromObject(const Object& object, mesh::MediaRelayBudgetConfig& budget) {
  auto read_bps = [&](const char* key, int64_t& out) {
    if (!object.contains(key) || object.isNull(key)) {
      return;
    }
    if (auto value = ReadI64(object, key)) {
      out = *value;
    }
  };
  read_bps("node_capacity_up_bps", budget.node_capacity_up_bps);
  read_bps("node_capacity_down_bps", budget.node_capacity_down_bps);
  read_bps("max_session_up_bps", budget.max_session_up_bps);
  read_bps("max_session_down_bps", budget.max_session_down_bps);
  read_bps("default_per_user_up_bps", budget.default_per_user_up_bps);
  read_bps("default_per_user_down_bps", budget.default_per_user_down_bps);
}
} // namespace

Object ServiceEndpointToObject(const ServiceEndpointConfig& endpoint) {
  Object object;
  object.set("base_url", endpoint.base_url);
  object.set("transport", endpoint.transport);
  if (!endpoint.relay_id.empty()) {
    object.set("relay_id", endpoint.relay_id);
  }
  return object;
}

void ServiceEndpointFromObject(const Object& object, ServiceEndpointConfig& endpoint) {
  if (auto base_url = object.getString("base_url")) {
    endpoint.base_url = *base_url;
  }
  if (auto transport = object.getString("transport")) {
    endpoint.transport = *transport;
  }
  if (auto relay_id = object.getString("relay_id")) {
    endpoint.relay_id = *relay_id;
  }
}

Object DirectoryConfigToObject(const DirectoryConfig& config) {
  Object object;
  object.set("base_url", config.base_url);
  object.set("transport", config.transport);
  if (!config.providers.empty()) {
    std::vector<Value> providers;
    providers.reserve(config.providers.size());
    for (const ServiceEndpointConfig& provider : config.providers) {
      providers.push_back(ObjectValue(ServiceEndpointToObject(provider)));
    }
    object.set("providers", makeArray(std::move(providers)));
  }
  return object;
}

void DirectoryConfigFromObject(const Object& object, DirectoryConfig& config) {
  if (auto base_url = object.getString("base_url")) {
    config.base_url = *base_url;
  }
  if (auto transport = object.getString("transport")) {
    config.transport = *transport;
  }
  if (const Array* providers = object.getArray("providers")) {
    config.providers.clear();
    for (const Value& item : providers->elements) {
      const Object* row = asObject(item);
      if (!row) {
        continue;
      }
      ServiceEndpointConfig provider;
      ServiceEndpointFromObject(*row, provider);
      if (!provider.base_url.empty()) {
        config.providers.push_back(std::move(provider));
      }
    }
  }
  NormalizeDirectoryConfig(config);
}

Object MeshCapabilitiesToObject(const mesh::MeshCapabilities& caps) {
  Object object;
  object.set("circuit_relay", caps.circuit_relay);
  object.set("media_relay", caps.media_relay);
  object.set("dht", caps.dht);
  object.set("ledger_gateway", caps.ledger_gateway);
  return object;
}

void MeshCapabilitiesFromObject(const Object& object, mesh::MeshCapabilities& caps) {
  if (auto circuit_relay = object.getIf<bool>("circuit_relay")) {
    caps.circuit_relay = *circuit_relay;
  }
  if (auto media_relay = object.getIf<bool>("media_relay")) {
    caps.media_relay = *media_relay;
  }
  if (auto dht = object.getIf<bool>("dht")) {
    caps.dht = *dht;
  }
  if (auto ledger_gateway = object.getIf<bool>("ledger_gateway")) {
    caps.ledger_gateway = *ledger_gateway;
  }
}

Object MeshDhtConfigToObject(const mesh::MeshDhtConfig& config) {
  Object object;
  object.set("record_ttl_seconds", static_cast<int64_t>(config.record_ttl_seconds));
  object.set("find_peer_timeout_ms", static_cast<int64_t>(config.find_peer_timeout_ms));
  object.set("max_concurrent_lookups", static_cast<int64_t>(config.max_concurrent_lookups));
  object.set("k_bucket_size", static_cast<int64_t>(config.k_bucket_size));
  object.set("inbound_ops_per_peer_per_window", static_cast<int64_t>(config.inbound_ops_per_peer_per_window));
  object.set("inbound_rate_window_seconds", static_cast<int64_t>(config.inbound_rate_window_seconds));
  object.set("soft_reputation_penalty_threshold", static_cast<int64_t>(config.soft_reputation_penalty_threshold));
  object.set("soft_reputation_cooldown_seconds", static_cast<int64_t>(config.soft_reputation_cooldown_seconds));
  return object;
}

void MeshDhtConfigFromObject(const Object& object, mesh::MeshDhtConfig& config) {
  if (auto ttl = object.getNonNegInt("record_ttl_seconds")) {
    config.record_ttl_seconds = static_cast<int>(*ttl);
  }
  if (auto timeout = object.getNonNegInt("find_peer_timeout_ms")) {
    config.find_peer_timeout_ms = static_cast<int>(*timeout);
  }
  if (auto max_lookup = object.getNonNegInt("max_concurrent_lookups")) {
    config.max_concurrent_lookups = static_cast<int>(*max_lookup);
  }
  if (auto k = object.getNonNegInt("k_bucket_size")) {
    config.k_bucket_size = static_cast<int>(*k);
  }
  if (auto ops = object.getNonNegInt("inbound_ops_per_peer_per_window")) {
    config.inbound_ops_per_peer_per_window = static_cast<int>(*ops);
  }
  if (auto window = object.getNonNegInt("inbound_rate_window_seconds")) {
    config.inbound_rate_window_seconds = static_cast<int>(*window);
  }
  if (auto threshold = object.getNonNegInt("soft_reputation_penalty_threshold")) {
    config.soft_reputation_penalty_threshold = static_cast<int>(*threshold);
  }
  if (auto cooldown = object.getNonNegInt("soft_reputation_cooldown_seconds")) {
    config.soft_reputation_cooldown_seconds = static_cast<int>(*cooldown);
  }
}

Object MeshConfigToObject(const MeshConfig& config) {
  std::vector<Value> peers;
  peers.reserve(config.bootstrap_peers.size());
  for (const std::string& peer : config.bootstrap_peers) {
    peers.emplace_back(peer);
  }
  std::vector<Value> advertise;
  advertise.reserve(config.advertise_multiaddrs.size());
  for (const std::string& ma : config.advertise_multiaddrs) {
    advertise.emplace_back(ma);
  }

  Object object;
  object.set("node_enabled", config.node_enabled);
  object.set("bootstrap_peers", makeArray(std::move(peers)));
  object.set("advertise_multiaddrs", makeArray(std::move(advertise)));
  object.set("mesh_publish", config.mesh_publish);
  object.set("mesh_heartbeat", config.mesh_heartbeat);
  object.set("prefer_contacts_for_routing", config.prefer_contacts_for_routing);
  object.set("direct_connections", mesh::DirectAudienceName(config.direct_connections));
  object.set("trusted_relays_only", config.trusted_relays_only);
  if (!config.preferred_min_node_version.empty()) {
    object.set("preferred_min_node_version", config.preferred_min_node_version);
  }
  object.set("reachable_without_relay", config.reachable_without_relay);
  object.set("mesh_enabled", config.mesh_enabled);
  object.set("amp_udp_port", static_cast<int64_t>(config.amp_udp_port));
  object.set("mobility", config.mobility);
  object.set("capabilities", MeshCapabilitiesToObject(config.capabilities));
  object.set("dht", MeshDhtConfigToObject(config.dht));
  object.set("pricing", MeshPricingToObject(config.pricing));
  object.set("media_relay_budget", MediaRelayBudgetToObject(config.media_relay_budget));
  object.set("capacity", MeshCapacityToObject(config.capacity));
  object.set("media_relay_video", MediaRelayVideoToObject(config.media_relay_video));
  return object;
}

void MeshConfigFromObject(const Object& object, MeshConfig& config) {
  if (auto node_enabled = object.getIf<bool>("node_enabled")) {
    config.node_enabled = *node_enabled;
  }
  if (const Array* peers = object.getArray("bootstrap_peers")) {
    config.bootstrap_peers.clear();
    for (const Value& item : peers->elements) {
      if (auto peer = asString(item)) {
        config.bootstrap_peers.push_back(*peer);
      }
    }
  }
  if (const Array* advertise = object.getArray("advertise_multiaddrs")) {
    config.advertise_multiaddrs.clear();
    for (const Value& item : advertise->elements) {
      if (auto ma = asString(item)) {
        config.advertise_multiaddrs.push_back(*ma);
      }
    }
  }
  if (auto mesh_publish = object.getIf<bool>("mesh_publish")) {
    config.mesh_publish = *mesh_publish;
  }
  if (auto mesh_heartbeat = object.getIf<bool>("mesh_heartbeat")) {
    config.mesh_heartbeat = *mesh_heartbeat;
  }
  if (auto prefer = object.getIf<bool>("prefer_contacts_for_routing")) {
    config.prefer_contacts_for_routing = *prefer;
  }
  if (auto reachable = object.getIf<bool>("reachable_without_relay")) {
    config.reachable_without_relay = *reachable;
  }
  if (auto trusted = object.getIf<bool>("trusted_relays_only")) {
    config.trusted_relays_only = *trusted;
  }
  if (auto floor = object.getString("preferred_min_node_version")) {
    config.preferred_min_node_version = *floor;
  }
  if (auto audience = object.getString("direct_connections")) {
    if (auto parsed = mesh::DirectAudienceFromName(*audience)) {
      config.direct_connections = *parsed;
    }
  }
  if (auto mesh_enabled = object.getIf<bool>("mesh_enabled")) {
    config.mesh_enabled = *mesh_enabled;
  }
  if (auto amp_udp_port = object.getNonNegInt("amp_udp_port")) {
    config.amp_udp_port = static_cast<int>(*amp_udp_port);
  }
  if (auto mobility = object.getString("mobility")) {
    config.mobility = *mobility;
  }
  if (const Object* capabilities = object.getObject("capabilities")) {
    MeshCapabilitiesFromObject(*capabilities, config.capabilities);
  }
  if (const Object* dht = object.getObject("dht")) {
    MeshDhtConfigFromObject(*dht, config.dht);
  }
  if (const Object* pricing = object.getObject("pricing")) {
    MeshPricingFromObject(*pricing, config.pricing);
  }
  if (const Object* media_relay_budget = object.getObject("media_relay_budget")) {
    MediaRelayBudgetFromObject(*media_relay_budget, config.media_relay_budget);
  }
  if (const Object* capacity = object.getObject("capacity")) {
    MeshCapacityFromObject(*capacity, config.capacity);
  }
  if (const Object* media_relay_video = object.getObject("media_relay_video")) {
    MediaRelayVideoFromObject(*media_relay_video, config.media_relay_video);
  }
}

/** `backend.services`: only the services this file names, so a config that names none writes no key. */
void BackendServicesFromObject(const Object& object, CustomServices& services) {
  const auto read = [&object](const char* key, std::string& out) {
    if (auto value = object.getString(key)) {
      out = *value;
    }
  };
  read("account_locker", services.account_locker_url);
  read("client_compat", services.client_compat_url);
  read("llm_gateway", services.llm_gateway_url);
  read("llm_token", services.llm_token_url);
  read("promoted_mcp", services.promoted_mcp_url);
  read("crash_intake", services.crash_intake_url);
  read("mesh_nodes", services.mesh_nodes_url);
}

Object BackendServicesToObject(const CustomServices& services) {
  Object object;
  const auto write = [&object](const char* key, const std::string& value) {
    if (!value.empty()) {
      object.set(key, value);
    }
  };
  write("account_locker", services.account_locker_url);
  write("client_compat", services.client_compat_url);
  write("llm_gateway", services.llm_gateway_url);
  write("llm_token", services.llm_token_url);
  write("promoted_mcp", services.promoted_mcp_url);
  write("crash_intake", services.crash_intake_url);
  write("mesh_nodes", services.mesh_nodes_url);
  return object;
}

CustomServices CustomServicesFromObject(const Object& root) {
  CustomServices services;
  if (const Object* backend = root.getObject("backend")) {
    if (const Object* extra = backend->getObject("services")) {
      BackendServicesFromObject(*extra, services);
    }
  }
  if (const Object* relay_object = root.getObject("relay")) {
    ServiceEndpointConfig relay;
    ServiceEndpointFromObject(*relay_object, relay);
    services.org_relay_url = relay.base_url;
    services.org_relay_id = relay.relay_id;
    if (const Array* extras = relay_object->getArray("extra_base_urls")) {
      for (const Value& item : extras->elements) {
        if (auto url = asString(item); url && !url->empty()) {
          services.seed_relays.push_back(*url);
        }
      }
    }
  }
  if (const Object* registration_object = root.getObject("registration")) {
    ServiceEndpointConfig registration;
    ServiceEndpointFromObject(*registration_object, registration);
    services.registration_url = registration.base_url;
    services.registration_relay_id = registration.relay_id;
  }
  if (const Object* directory_object = root.getObject("directory")) {
    DirectoryConfig directory;
    DirectoryConfigFromObject(*directory_object, directory);
    for (const ServiceEndpointConfig& provider : EffectiveDirectoryProviders(directory)) {
      services.directory_providers.push_back(provider.base_url);
    }
  }
  return services;
}

} // namespace pbr
